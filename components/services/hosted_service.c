#include "hosted_service.h"

#include <string.h>

#include "esp_hosted.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hosted_service";
static SemaphoreHandle_t s_mutex;
static ephoto_hosted_status_t s_status;
static bool s_upgrade_attempted_once;
static int64_t s_init_ms;
static bool s_refresh_suspended;

#ifndef EPHOTO_HAS_EMBEDDED_HOSTED_FW
#define EPHOTO_HAS_EMBEDDED_HOSTED_FW 0
#endif

#define HOSTED_REFRESH_INTERVAL_MS       15000
#define HOSTED_REFRESH_RETRY_INTERVAL_MS 2000
#define HOSTED_OTA_CHUNK_SIZE            1500
#define HOSTED_BOOT_GRACE_MS             15000

#if EPHOTO_HAS_EMBEDDED_HOSTED_FW && CONFIG_SLAVE_IDF_TARGET_ESP32C5
extern const uint8_t ESP32C5_2_12_9_0X0_bin_start[] asm("_binary_ESP32C5_2_12_9_0X0_bin_start");
extern const uint8_t ESP32C5_2_12_9_0X0_bin_end[] asm("_binary_ESP32C5_2_12_9_0X0_bin_end");
#elif EPHOTO_HAS_EMBEDDED_HOSTED_FW && CONFIG_SLAVE_IDF_TARGET_ESP32C6
extern const uint8_t ESP32C6_2_12_9_0X0_bin_start[] asm("_binary_ESP32C6_2_12_9_0X0_bin_start");
extern const uint8_t ESP32C6_2_12_9_0X0_bin_end[] asm("_binary_ESP32C6_2_12_9_0X0_bin_end");
#endif

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int compare_versions(uint32_t host_major,
                            uint32_t host_minor,
                            uint32_t host_patch,
                            uint32_t slave_major,
                            uint32_t slave_minor,
                            uint32_t slave_patch)
{
    if (host_major == slave_major && host_minor == slave_minor && host_patch == slave_patch) {
        return 0;
    }
    if (host_major > slave_major ||
        (host_major == slave_major && host_minor > slave_minor) ||
        (host_major == slave_major && host_minor == slave_minor && host_patch > slave_patch)) {
        return -1;
    }
    return 1;
}

static esp_err_t perform_embedded_ota(void)
{
#if EPHOTO_HAS_EMBEDDED_HOSTED_FW && CONFIG_SLAVE_IDF_TARGET_ESP32C5
    const uint8_t *image = ESP32C5_2_12_9_0X0_bin_start;
    size_t image_size = (size_t)(ESP32C5_2_12_9_0X0_bin_end - ESP32C5_2_12_9_0X0_bin_start);
#elif EPHOTO_HAS_EMBEDDED_HOSTED_FW && CONFIG_SLAVE_IDF_TARGET_ESP32C6
    const uint8_t *image = ESP32C6_2_12_9_0X0_bin_start;
    size_t image_size = (size_t)(ESP32C6_2_12_9_0X0_bin_end - ESP32C6_2_12_9_0X0_bin_start);
#endif

#if EPHOTO_HAS_EMBEDDED_HOSTED_FW && (CONFIG_SLAVE_IDF_TARGET_ESP32C5 || CONFIG_SLAVE_IDF_TARGET_ESP32C6)
    if (!image || image_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGW(TAG,
             "starting embedded hosted OTA for packaged slave image size=%u bytes",
             (unsigned)image_size);

    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t offset = 0; offset < image_size; offset += HOSTED_OTA_CHUNK_SIZE) {
        size_t chunk_size = image_size - offset;
        if (chunk_size > HOSTED_OTA_CHUNK_SIZE) {
            chunk_size = HOSTED_OTA_CHUNK_SIZE;
        }

        err = esp_hosted_slave_ota_write((uint8_t *)(image + offset), (uint32_t)chunk_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "esp_hosted_slave_ota_write failed at offset=%u: %s",
                     (unsigned)offset,
                     esp_err_to_name(err));
            return err;
        }
    }

    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
#else
    ESP_LOGW(TAG, "embedded hosted OTA is not packaged for the current slave target");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t hosted_service_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.available = true;
    s_status.host_major = ESP_HOSTED_VERSION_MAJOR_1;
    s_status.host_minor = ESP_HOSTED_VERSION_MINOR_1;
    s_status.host_patch = ESP_HOSTED_VERSION_PATCH_1;
    s_status.last_error = ESP_ERR_INVALID_STATE;
    s_upgrade_attempted_once = false;
    s_init_ms = now_ms();
    s_refresh_suspended = false;
    return ESP_OK;
}

esp_err_t hosted_service_refresh(bool force)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_refresh_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t previous_error = ESP_ERR_INVALID_STATE;
    bool had_successful_query = false;
    uint32_t prev_slave_major = 0;
    uint32_t prev_slave_minor = 0;
    uint32_t prev_slave_patch = 0;
    int32_t prev_relation = 0;
    bool prev_needs_upgrade = false;
    bool prev_slave_ota_supported = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    previous_error = s_status.last_error;
    had_successful_query = s_status.queried && (s_status.last_error == ESP_OK);
    prev_slave_major = s_status.slave_major;
    prev_slave_minor = s_status.slave_minor;
    prev_slave_patch = s_status.slave_patch;
    prev_relation = s_status.version_relation;
    prev_needs_upgrade = s_status.needs_upgrade;
    prev_slave_ota_supported = s_status.slave_ota_supported;
    int64_t elapsed_ms = now_ms() - s_status.last_query_ms;
    int64_t min_refresh_interval_ms =
        (previous_error == ESP_OK) ? HOSTED_REFRESH_INTERVAL_MS : HOSTED_REFRESH_RETRY_INTERVAL_MS;
    if (!force && s_status.queried && elapsed_ms >= 0 && elapsed_ms < min_refresh_interval_ms) {
        esp_err_t cached = s_status.last_error;
        xSemaphoreGive(s_mutex);
        return cached;
    }
    xSemaphoreGive(s_mutex);

    esp_hosted_coprocessor_fwver_t version = {0};
    esp_err_t err = esp_hosted_get_coprocessor_fwversion(&version);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.available = true;
    s_status.queried = true;
    s_status.last_query_ms = now_ms();
    s_status.last_error = err;

    if (err == ESP_OK) {
        s_status.slave_major = version.major1;
        s_status.slave_minor = version.minor1;
        s_status.slave_patch = version.patch1;
        s_status.version_relation = compare_versions(s_status.host_major,
                                                     s_status.host_minor,
                                                     s_status.host_patch,
                                                     s_status.slave_major,
                                                     s_status.slave_minor,
                                                     s_status.slave_patch);
        s_status.needs_upgrade = s_status.version_relation < 0;
        s_status.slave_ota_supported = (version.major1 > 2) ||
                                       (version.major1 == 2 && version.minor1 > 5);
    } else {
        if (had_successful_query) {
            s_status.slave_major = prev_slave_major;
            s_status.slave_minor = prev_slave_minor;
            s_status.slave_patch = prev_slave_patch;
            s_status.version_relation = prev_relation;
            s_status.needs_upgrade = prev_needs_upgrade;
            s_status.slave_ota_supported = prev_slave_ota_supported;
        } else {
            s_status.slave_major = 0;
            s_status.slave_minor = 0;
            s_status.slave_patch = 0;
            s_status.version_relation = 0;
            s_status.needs_upgrade = false;
            s_status.slave_ota_supported = false;
        }
        if ((now_ms() - s_init_ms) < HOSTED_BOOT_GRACE_MS) {
            ESP_LOGI(TAG, "co-processor version query pending: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "co-processor version query failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

void hosted_service_set_refresh_suspended(bool suspended)
{
    s_refresh_suspended = suspended;
}

bool hosted_service_is_ready(void)
{
    if (!s_mutex) {
        return false;
    }

    bool ready = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ready = s_status.queried && (s_status.last_error == ESP_OK);
    xSemaphoreGive(s_mutex);
    return ready;
}

void hosted_service_get_status(ephoto_hosted_status_t *out_status)
{
    if (!out_status) {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out_status = s_status;
    xSemaphoreGive(s_mutex);
}

#if EPHOTO_ENABLE_OTA && EPHOTO_HAS_EMBEDDED_HOSTED_FW

esp_err_t hosted_service_upgrade_embedded_if_needed(void)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool already_attempted = s_upgrade_attempted_once;
    bool has_valid_query = s_status.queried && (s_status.last_error == ESP_OK);
    bool needs_upgrade = s_status.needs_upgrade;
    uint32_t slave_major = s_status.slave_major;
    uint32_t slave_minor = s_status.slave_minor;
    uint32_t slave_patch = s_status.slave_patch;
    bool can_activate = s_status.slave_ota_supported;
    xSemaphoreGive(s_mutex);

    if (already_attempted || !has_valid_query || !needs_upgrade) {
        return ESP_OK;
    }

    s_upgrade_attempted_once = true;

    ESP_LOGW(TAG,
             "embedded hosted OTA is required: slave=%lu.%lu.%lu host=%lu.%lu.%lu",
             (unsigned long)slave_major,
             (unsigned long)slave_minor,
             (unsigned long)slave_patch,
             (unsigned long)ESP_HOSTED_VERSION_MAJOR_1,
             (unsigned long)ESP_HOSTED_VERSION_MINOR_1,
             (unsigned long)ESP_HOSTED_VERSION_PATCH_1);

    esp_err_t err = perform_embedded_ota();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "embedded hosted OTA failed: %s", esp_err_to_name(err));
        return err;
    }

    if (can_activate) {
        esp_err_t activate_err = esp_hosted_slave_ota_activate();
        if (activate_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hosted_slave_ota_activate failed: %s", esp_err_to_name(activate_err));
            return activate_err;
        }
        ESP_LOGW(TAG, "embedded hosted OTA activated, slave reboot requested");
    } else {
        ESP_LOGW(TAG, "embedded hosted OTA finished, current slave does not require activate API");
    }

    ESP_LOGW(TAG, "restarting host to resync with upgraded hosted firmware");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

#else /* The original hosted OTA implementation remains above but is disabled in the public build. */

esp_err_t hosted_service_upgrade_embedded_if_needed(void)
{
    /* Embedded hosted firmware upgrade is disabled in the public build. */
    return ESP_OK;
}

#endif
