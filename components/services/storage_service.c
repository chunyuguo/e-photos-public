#include "storage_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_interface.h"

static const char *TAG = "storage_service";
static const board_profile_t *s_profile;
static sdmmc_card_t *s_card;
static ephoto_storage_status_t s_status;
static sd_pwr_ctrl_handle_t s_pwr_ctrl;
static int s_mounted_width;
static int s_mounted_freq_khz;

#define EPHOTO_SD_BENCH_TOTAL_BYTES (4 * 1024 * 1024)
#define EPHOTO_SD_BENCH_CHUNK_BYTES (128 * 1024)
#define EPHOTO_SD_FREQ_PRIMARY_KHZ 40000
#define EPHOTO_SD_FREQ_NORMAL_KHZ 20000
#define EPHOTO_SD_FREQ_SAFE_FALLBACK_KHZ 10000
#define EPHOTO_SD_IO_VOLTAGE_MV 3300

typedef struct {
    esp_ldo_channel_handle_t channel;
    int voltage_mv;
} storage_sd_ldo_ctx_t;

typedef struct {
    int width;
    int freq_khz;
    const char *label;
} sd_mount_attempt_t;

static esp_err_t storage_sd_ldo_set_voltage(void *ctx, int voltage_mv)
{
    storage_sd_ldo_ctx_t *ldo_ctx = (storage_sd_ldo_ctx_t *)ctx;
    ESP_RETURN_ON_FALSE(ldo_ctx && ldo_ctx->channel, ESP_ERR_INVALID_ARG, TAG, "invalid sd ldo ctx");
    ESP_RETURN_ON_ERROR(esp_ldo_channel_adjust_voltage(ldo_ctx->channel, voltage_mv),
                        TAG,
                        "failed to adjust TF card LDO voltage");
    ldo_ctx->voltage_mv = voltage_mv;
    return ESP_OK;
}

static esp_err_t storage_create_sd_ldo_power_ctrl(int ldo_chan_id, sd_pwr_ctrl_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(ldo_chan_id >= 0 && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid sd ldo arg");

    sd_pwr_ctrl_drv_t *driver = calloc(1, sizeof(*driver));
    storage_sd_ldo_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!driver || !ctx) {
        free(driver);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = ldo_chan_id,
        .voltage_mv = EPHOTO_SD_IO_VOLTAGE_MV,
        .flags.adjustable = true,
    };
    esp_ldo_channel_handle_t channel = NULL;
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &channel);
    if (err != ESP_OK) {
        free(driver);
        free(ctx);
        return err;
    }

    ctx->channel = channel;
    ctx->voltage_mv = EPHOTO_SD_IO_VOLTAGE_MV;
    driver->ctx = ctx;
    driver->set_io_voltage = storage_sd_ldo_set_voltage;
    *out_handle = driver;
    return ESP_OK;
}

static bool is_card_detected(void)
{
    if (!s_profile || s_profile->sd_det == GPIO_NUM_NC) {
        return true;
    }
    int level = gpio_get_level(s_profile->sd_det);
    return s_profile->sd_det_active_low ? (level == 0) : (level != 0);
}

static esp_err_t mount_sdmmc_with_width_freq(int width, int freq_khz)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = s_profile->sdmmc_slot;
    host.max_freq_khz = freq_khz;
    host.io_voltage = 3.3f;
    host.pwr_ctrl_handle = s_pwr_ctrl;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = width;
    slot_config.clk = s_profile->sd_clk;
    slot_config.cmd = s_profile->sd_cmd;
    slot_config.d0 = s_profile->sd_d0;
    slot_config.d1 = width >= 4 ? s_profile->sd_d1 : GPIO_NUM_NC;
    slot_config.d2 = width >= 4 ? s_profile->sd_d2 : GPIO_NUM_NC;
    slot_config.d3 = width >= 4 ? s_profile->sd_d3 : GPIO_NUM_NC;
    if (s_profile->sdmmc_internal_pullups) {
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };

    ESP_LOGI(TAG,
             "mounting TF card: slot=%d width=%d freq=%dkHz clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d",
             host.slot,
             width,
             freq_khz,
             slot_config.clk,
             slot_config.cmd,
             slot_config.d0,
             slot_config.d1,
             slot_config.d2,
             slot_config.d3);

    return esp_vfs_fat_sdmmc_mount(s_profile->mount_path, &host, &slot_config, &mount_config, &s_card);
}

static void ensure_photo_dir(void)
{
    struct stat st;
    if (stat(s_profile->photo_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        return;
    }
    mkdir(s_profile->photo_dir, 0775);
}

static void clear_mount_state(void)
{
    s_card = NULL;
    s_status.mounted = false;
    s_status.capacity_bytes = 0;
    s_status.used_bytes = 0;
    s_mounted_width = 0;
    s_mounted_freq_khz = 0;
}

static void release_sd_power_ctrl(void)
{
    if (!s_pwr_ctrl) {
        return;
    }

    storage_sd_ldo_ctx_t *ctx = (storage_sd_ldo_ctx_t *)s_pwr_ctrl->ctx;
    if (ctx && ctx->channel) {
        esp_err_t err = esp_ldo_release_channel(ctx->channel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "release TF card LDO channel failed: %s", esp_err_to_name(err));
        }
    }

    free(ctx);
    free(s_pwr_ctrl);
    s_pwr_ctrl = NULL;
}

esp_err_t storage_service_init(const board_profile_t *profile)
{
    s_profile = profile;
    memset(&s_status, 0, sizeof(s_status));
    s_status.present = is_card_detected();

    if (!s_status.present) {
        s_status.last_error = ESP_ERR_NOT_FOUND;
        ESP_LOGW(TAG, "TF card not detected on GPIO%d", (int)profile->sd_det);
        return ESP_ERR_NOT_FOUND;
    }

    if (profile->sd_pwr_ldo_chan >= 0 && s_pwr_ctrl == NULL) {
        esp_err_t pwr_err = storage_create_sd_ldo_power_ctrl(profile->sd_pwr_ldo_chan, &s_pwr_ctrl);
        if (pwr_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to enable TF card LDO channel %d: %s",
                     profile->sd_pwr_ldo_chan, esp_err_to_name(pwr_err));
        } else {
            ESP_LOGI(TAG,
                     "enabled TF card power via on-chip LDO channel %d at %dmV",
                     profile->sd_pwr_ldo_chan,
                     EPHOTO_SD_IO_VOLTAGE_MV);
        }
    }

    // Test the 1-bit / 40MHz path while keeping conservative fallbacks.
    // A card can pass mount-time negotiation at 4-bit and still lose write
    // commands under sustained Web uploads, which surfaces as SDMMC 0x107/
    // 0x109 errors. Keep 4-bit as a fallback for cards with a clean bus.
    s_mounted_width = 0;
    s_mounted_freq_khz = 0;
    esp_err_t err = ESP_FAIL;
    sd_mount_attempt_t attempts[5] = {
        {.width = 1, .freq_khz = EPHOTO_SD_FREQ_PRIMARY_KHZ, .label = "primary-1bit-40m"},
        {.width = 1, .freq_khz = EPHOTO_SD_FREQ_NORMAL_KHZ, .label = "fallback-1bit-20m"},
        {.width = 1, .freq_khz = EPHOTO_SD_FREQ_SAFE_FALLBACK_KHZ, .label = "fallback-1bit-10m"},
        {.width = 4, .freq_khz = EPHOTO_SD_FREQ_NORMAL_KHZ, .label = "fallback-4bit-20m"},
        {.width = 1, .freq_khz = EPHOTO_SD_FREQ_SAFE_FALLBACK_KHZ, .label = "safe-1bit-10m"},
    };
    size_t attempt_count = sizeof(attempts) / sizeof(attempts[0]);

    for (size_t i = 0; i < attempt_count; ++i) {
        const sd_mount_attempt_t *attempt = &attempts[i];
        ESP_LOGI(TAG,
                 "trying TF card mount profile=%s width=%d freq=%dkHz",
                 attempt->label ? attempt->label : "-",
                 attempt->width,
                 attempt->freq_khz);
        err = mount_sdmmc_with_width_freq(attempt->width, attempt->freq_khz);
        if (err == ESP_OK) {
            s_mounted_width = attempt->width;
            s_mounted_freq_khz = attempt->freq_khz;
            break;
        }
        ESP_LOGW(TAG,
                 "TF card mount profile=%s failed: %s",
                 attempt->label ? attempt->label : "-",
                 esp_err_to_name(err));
    }
    s_status.last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        s_status.present = false;
        s_status.mounted = false;
        return err;
    }

    s_status.present = true;
    s_status.mounted = true;
    int real_freq_khz = 0;
    if (sdmmc_host_get_real_freq(profile->sdmmc_slot, &real_freq_khz) == ESP_OK) {
        s_mounted_freq_khz = real_freq_khz;
        ESP_LOGI(TAG, "TF card mounted: bus_width=%d real_freq=%dkHz", s_mounted_width, real_freq_khz);
    } else {
        ESP_LOGI(TAG, "TF card mounted: bus_width=%d target_freq=%dkHz", s_mounted_width, s_mounted_freq_khz);
    }
    ensure_photo_dir();
    return storage_service_refresh_stats();
}

void storage_service_deinit(void)
{
    if (s_card && s_profile && s_status.mounted) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(s_profile->mount_path, s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TF card deinit unmount failed: %s", esp_err_to_name(err));
        }
    }

    clear_mount_state();
    release_sd_power_ctrl();
    s_profile = NULL;
}

esp_err_t storage_service_refresh_stats(void)
{
    if (!s_profile || !s_status.mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0;
    uint64_t free = 0;
    esp_err_t err = esp_vfs_fat_info(s_profile->mount_path, &total, &free);
    if (err == ESP_OK) {
        s_status.capacity_bytes = total;
        s_status.used_bytes = total - free;
    }
    s_status.last_error = err;
    return err;
}

void storage_service_get_status(ephoto_storage_status_t *out_status)
{
    s_status.present = is_card_detected();
    if (!s_status.present) {
        s_status.mounted = false;
        s_status.last_error = ESP_ERR_NOT_FOUND;
    }
    *out_status = s_status;
}

const char *storage_service_get_mount_path(void)
{
    return s_profile ? s_profile->mount_path : "";
}

const char *storage_service_get_photo_dir(void)
{
    return s_profile ? s_profile->photo_dir : "";
}

int storage_service_get_mounted_bus_width(void)
{
    return s_mounted_width;
}

int storage_service_get_mounted_freq_khz(void)
{
    return s_mounted_freq_khz;
}

esp_err_t storage_service_run_benchmark(ephoto_storage_benchmark_t *out_result)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->bytes_tested = EPHOTO_SD_BENCH_TOTAL_BYTES;

    if (!s_profile || !s_status.mounted) {
        out_result->error = ESP_ERR_INVALID_STATE;
        return out_result->error;
    }

    char path[EPHOTO_MAX_MOUNT_PATH_LEN + 32] = {0};
    if (snprintf(path, sizeof(path), "%s/.sdbench.tmp", s_profile->mount_path) >= (int)sizeof(path)) {
        out_result->error = ESP_ERR_INVALID_SIZE;
        return out_result->error;
    }

    uint8_t *buffer = malloc(EPHOTO_SD_BENCH_CHUNK_BYTES);
    if (!buffer) {
        out_result->error = ESP_ERR_NO_MEM;
        return out_result->error;
    }
    for (size_t i = 0; i < EPHOTO_SD_BENCH_CHUNK_BYTES; ++i) {
        buffer[i] = (uint8_t)(i & 0xFF);
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        free(buffer);
        out_result->error = ESP_FAIL;
        return out_result->error;
    }

    int64_t write_start_ms = esp_timer_get_time() / 1000;
    size_t remaining = EPHOTO_SD_BENCH_TOTAL_BYTES;
    while (remaining > 0) {
        size_t chunk = remaining > EPHOTO_SD_BENCH_CHUNK_BYTES ? EPHOTO_SD_BENCH_CHUNK_BYTES : remaining;
        if (fwrite(buffer, 1, chunk, file) != chunk) {
            fclose(file);
            unlink(path);
            free(buffer);
            out_result->error = ESP_FAIL;
            return out_result->error;
        }
        remaining -= chunk;
    }
    fflush(file);
    fsync(fileno(file));
    fclose(file);
    int64_t write_end_ms = esp_timer_get_time() / 1000;

    file = fopen(path, "rb");
    if (!file) {
        unlink(path);
        free(buffer);
        out_result->error = ESP_FAIL;
        return out_result->error;
    }

    int64_t read_start_ms = esp_timer_get_time() / 1000;
    remaining = EPHOTO_SD_BENCH_TOTAL_BYTES;
    while (remaining > 0) {
        size_t chunk = remaining > EPHOTO_SD_BENCH_CHUNK_BYTES ? EPHOTO_SD_BENCH_CHUNK_BYTES : remaining;
        if (fread(buffer, 1, chunk, file) != chunk) {
            fclose(file);
            unlink(path);
            free(buffer);
            out_result->error = ESP_FAIL;
            return out_result->error;
        }
        remaining -= chunk;
    }
    fclose(file);
    unlink(path);
    free(buffer);
    int64_t read_end_ms = esp_timer_get_time() / 1000;

    out_result->write_ms = write_end_ms - write_start_ms;
    out_result->read_ms = read_end_ms - read_start_ms;
    out_result->write_kib_per_s = out_result->write_ms > 0
                                      ? (uint32_t)(((uint64_t)out_result->bytes_tested * 1000ULL) /
                                                   ((uint64_t)out_result->write_ms * 1024ULL))
                                      : 0;
    out_result->read_kib_per_s = out_result->read_ms > 0
                                     ? (uint32_t)(((uint64_t)out_result->bytes_tested * 1000ULL) /
                                                  ((uint64_t)out_result->read_ms * 1024ULL))
                                     : 0;
    out_result->ok = true;
    out_result->error = ESP_OK;

    ESP_LOGI(TAG,
             "SD benchmark: width=%d freq=%dkHz bytes=%u write=%lldms(%u KiB/s) read=%lldms(%u KiB/s)",
             s_mounted_width,
             s_mounted_freq_khz,
             (unsigned)out_result->bytes_tested,
             out_result->write_ms,
             out_result->write_kib_per_s,
             out_result->read_ms,
             out_result->read_kib_per_s);
    return ESP_OK;
}

esp_err_t storage_service_unmount_for_benchmark(void)
{
    if (s_card && s_profile && s_status.mounted) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(s_profile->mount_path, s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TF card unmount failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    clear_mount_state();
    return ESP_OK;
}

esp_err_t storage_service_remount_for_benchmark(int width, int freq_khz)
{
    if (!s_profile) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_card_detected()) {
        s_status.present = false;
        s_status.last_error = ESP_ERR_NOT_FOUND;
        return ESP_ERR_NOT_FOUND;
    }

    storage_service_unmount_for_benchmark();

    esp_err_t err = mount_sdmmc_with_width_freq(width, freq_khz);
    s_status.last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "TF benchmark remount failed: width=%d freq=%dkHz err=%s",
                 width,
                 freq_khz,
                 esp_err_to_name(err));
        return err;
    }

    s_status.present = true;
    s_status.mounted = true;
    s_mounted_width = width;
    s_mounted_freq_khz = freq_khz;
    int real_freq_khz = 0;
    if (sdmmc_host_get_real_freq(s_profile->sdmmc_slot, &real_freq_khz) == ESP_OK) {
        s_mounted_freq_khz = real_freq_khz;
        ESP_LOGI(TAG, "TF card mounted: bus_width=%d real_freq=%dkHz", s_mounted_width, real_freq_khz);
    } else {
        ESP_LOGI(TAG, "TF card mounted: bus_width=%d target_freq=%dkHz", s_mounted_width, s_mounted_freq_khz);
    }
    ensure_photo_dir();
    return storage_service_refresh_stats();
}
