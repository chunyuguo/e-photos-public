#include "boot_image_service.h"

#include <string.h>

#include "clock_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "boot_image_service";
static const char *NAMESPACE = "bootimg";
static const char *KEY_SOURCE = "source";
static const char *KEY_NAME = "name";

#define EPHOTO_BOOT_IMAGE_WIDTH 1280U
#define EPHOTO_BOOT_IMAGE_HEIGHT 800U
#define EPHOTO_BOOT_IMAGE_SIZE_BYTES ((size_t)EPHOTO_BOOT_IMAGE_WIDTH * EPHOTO_BOOT_IMAGE_HEIGHT * 2U)
#define EPHOTO_BOOT_IMAGE_CUSTOM_LABEL "bootimg_b"

static ephoto_boot_image_status_t s_status;
static const esp_partition_t *s_custom_partition;
static bool s_initialized;

static void fill_default_status(ephoto_boot_image_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->active_source = EPHOTO_BOOT_IMAGE_SOURCE_DEFAULT;
    strlcpy(status->active_name, "默认开机画面", sizeof(status->active_name));
}

static const esp_partition_t *find_custom_partition(void)
{
    if (!s_custom_partition) {
        s_custom_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                      ESP_PARTITION_SUBTYPE_ANY,
                                                      EPHOTO_BOOT_IMAGE_CUSTOM_LABEL);
    }
    return s_custom_partition;
}

static bool custom_partition_has_valid_size(void)
{
    const esp_partition_t *partition = find_custom_partition();
    return partition && partition->size >= EPHOTO_BOOT_IMAGE_SIZE_BYTES;
}

static bool custom_image_present_on_flash(void)
{
    const esp_partition_t *partition = find_custom_partition();
    if (!partition || partition->size < EPHOTO_BOOT_IMAGE_SIZE_BYTES) {
        return false;
    }

    uint32_t first_word = 0xFFFFFFFFU;
    if (esp_partition_read(partition, 0, &first_word, sizeof(first_word)) != ESP_OK) {
        return false;
    }
    return first_word != 0xFFFFFFFFU;
}

static void refresh_cached_status_from_nvs(void)
{
    fill_default_status(&s_status);
    s_status.custom_available = custom_image_present_on_flash();
    s_status.last_error = ESP_OK;

    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        s_status.custom_selected = false;
        s_status.active_source = EPHOTO_BOOT_IMAGE_SOURCE_DEFAULT;
        return;
    }

    uint8_t source = 0;
    if (nvs_get_u8(handle, KEY_SOURCE, &source) == ESP_OK &&
        source == (uint8_t)EPHOTO_BOOT_IMAGE_SOURCE_CUSTOM &&
        s_status.custom_available) {
        s_status.custom_selected = true;
        s_status.active_source = EPHOTO_BOOT_IMAGE_SOURCE_CUSTOM;
    }

    size_t name_len = sizeof(s_status.active_name);
    if (s_status.custom_selected &&
        nvs_get_str(handle, KEY_NAME, s_status.active_name, &name_len) != ESP_OK) {
        strlcpy(s_status.active_name, "自定义开机画面", sizeof(s_status.active_name));
    }
    nvs_close(handle);

    if (!s_status.custom_selected) {
        s_status.active_source = EPHOTO_BOOT_IMAGE_SOURCE_DEFAULT;
        strlcpy(s_status.active_name, "默认开机画面", sizeof(s_status.active_name));
    }
}

static esp_err_t save_selection_to_nvs(ephoto_boot_image_source_t source, const char *name)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "open nvs failed");
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, KEY_SOURCE, (uint8_t)source), done, TAG, "save source failed");
    if (name && name[0]) {
        ESP_GOTO_ON_ERROR(nvs_set_str(handle, KEY_NAME, name), done, TAG, "save name failed");
    } else {
        nvs_erase_key(handle, KEY_NAME);
    }
    ESP_GOTO_ON_ERROR(nvs_commit(handle), done, TAG, "commit boot source failed");
done:
    nvs_close(handle);
    return ret;
}

esp_err_t boot_image_service_init(void)
{
    refresh_cached_status_from_nvs();
    s_initialized = true;
    return custom_partition_has_valid_size() ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void boot_image_service_get_status(ephoto_boot_image_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    *out_status = s_status;
}

bool boot_image_service_is_custom_selected(void)
{
    return s_status.custom_selected && s_status.custom_available;
}

esp_err_t boot_image_service_factory_reset(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *partition = find_custom_partition();
    if (partition) {
        esp_err_t erase_err = esp_partition_erase_range(partition, 0, partition->size);
        if (erase_err != ESP_OK) {
            s_status.last_error = erase_err;
            return erase_err;
        }
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        (void)nvs_erase_all(handle);
        err = nvs_commit(handle);
        nvs_close(handle);
        if (err != ESP_OK) {
            s_status.last_error = err;
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        s_status.last_error = err;
        return err;
    }

    fill_default_status(&s_status);
    s_status.custom_available = false;
    s_status.custom_selected = false;
    s_status.updated_at_ms = clock_service_wall_time_ms();
    s_status.last_error = ESP_OK;
    return ESP_OK;
}

esp_err_t boot_image_service_use_default(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = save_selection_to_nvs(EPHOTO_BOOT_IMAGE_SOURCE_DEFAULT, NULL);
    if (err != ESP_OK) {
        s_status.last_error = err;
        return err;
    }

    s_status.custom_available = custom_image_present_on_flash();
    s_status.custom_selected = false;
    s_status.active_source = EPHOTO_BOOT_IMAGE_SOURCE_DEFAULT;
    strlcpy(s_status.active_name, "默认开机画面", sizeof(s_status.active_name));
    s_status.updated_at_ms = clock_service_wall_time_ms();
    s_status.last_error = ESP_OK;
    return ESP_OK;
}

esp_err_t boot_image_service_set_custom_rgb565(const char *display_name,
                                               const void *pixels,
                                               uint16_t width,
                                               uint16_t height)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pixels || width != EPHOTO_BOOT_IMAGE_WIDTH || height != EPHOTO_BOOT_IMAGE_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *partition = find_custom_partition();
    if (!partition || partition->size < EPHOTO_BOOT_IMAGE_SIZE_BYTES) {
        s_status.last_error = ESP_ERR_INVALID_SIZE;
        return ESP_ERR_INVALID_SIZE;
    }

    s_status.update_in_progress = true;
    s_status.last_error = ESP_OK;

    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err == ESP_OK) {
        err = esp_partition_write(partition, 0, pixels, EPHOTO_BOOT_IMAGE_SIZE_BYTES);
    }
    if (err == ESP_OK) {
        char title[EPHOTO_MAX_BOOT_IMAGE_NAME_LEN];
        strlcpy(title, (display_name && display_name[0]) ? display_name : "自定义开机画面", sizeof(title));
        err = save_selection_to_nvs(EPHOTO_BOOT_IMAGE_SOURCE_CUSTOM, title);
        if (err == ESP_OK) {
            s_status.custom_available = true;
            s_status.custom_selected = true;
            s_status.active_source = EPHOTO_BOOT_IMAGE_SOURCE_CUSTOM;
            strlcpy(s_status.active_name, title, sizeof(s_status.active_name));
            s_status.updated_at_ms = clock_service_wall_time_ms();
        }
    }

    s_status.update_in_progress = false;
    s_status.last_error = err;
    if (err != ESP_OK) {
        refresh_cached_status_from_nvs();
        s_status.last_error = err;
    }
    return err;
}
