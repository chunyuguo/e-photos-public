#include "settings_service.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings_service";
static const char *NAMESPACE = "settings";
static ephoto_settings_t s_cached_settings;

static void fill_defaults(const board_profile_t *profile, ephoto_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->playback_mode = EPHOTO_PLAYBACK_TIME_DESC;
    settings->slideshow_interval = EPHOTO_INTERVAL_10S;
    settings->brightness = EPHOTO_BRIGHTNESS_MAX;
    settings->manual_brightness = EPHOTO_BRIGHTNESS_MAX;
    settings->brightness_mode = EPHOTO_BRIGHTNESS_MODE_MANUAL;
    settings->clock_format = EPHOTO_CLOCK_FORMAT_24H;
    settings->clock_position = EPHOTO_CLOCK_POSITION_TOP_RIGHT;
    settings->clock_color = EPHOTO_CLOCK_COLOR_WHITE;
    settings->ota_interval = EPHOTO_OTA_INTERVAL_OFF;
    settings->fit_mode = EPHOTO_FIT_CONTAIN;
    settings->orientation_filter = EPHOTO_ORIENTATION_ALL;
    settings->clock_visible = true;
    settings->screen_on = true;
    settings->screen_schedule_enabled = false;
    settings->auto_rotation_enabled = false;
    settings->auto_time_sync = true;
    settings->screen_on_minute = 8U * 60U;
    settings->screen_off_minute = 23U * 60U;
    settings->rotation_deg = profile->default_rotation_deg;
    settings->manual_rotation_deg = profile->default_rotation_deg;
    strlcpy(settings->timezone, "CST-8", sizeof(settings->timezone));
    strlcpy(settings->ota_channel, "stable", sizeof(settings->ota_channel));
}

esp_err_t settings_service_init(const board_profile_t *profile)
{
    fill_defaults(profile, &s_cached_settings);
    esp_err_t err = settings_service_load(&s_cached_settings);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t settings_service_load(ephoto_settings_t *out_settings)
{
    if (!out_settings) {
        return ESP_ERR_INVALID_ARG;
    }

    // Always start from the cached/default settings so missing NVS keys after
    // factory reset cannot leave callers with zeroed fields like screen_on.
    *out_settings = s_cached_settings;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t u8_value = 0;
    uint16_t u16_value = 0;
    size_t tz_len = sizeof(out_settings->timezone);

    if (nvs_get_u8(handle, "mode", &u8_value) == ESP_OK) {
        out_settings->playback_mode = (ephoto_playback_mode_t)u8_value;
    }
    if (nvs_get_u8(handle, "interval", &u8_value) == ESP_OK) {
        out_settings->slideshow_interval = (ephoto_slideshow_interval_t)u8_value;
    }
    if (nvs_get_u8(handle, "bright_pct", &u8_value) == ESP_OK) {
        out_settings->manual_brightness = u8_value > EPHOTO_BRIGHTNESS_MAX
                                              ? EPHOTO_BRIGHTNESS_MAX
                                              : (ephoto_brightness_t)u8_value;
        out_settings->brightness = out_settings->manual_brightness;
    }
    if (nvs_get_u8(handle, "bright_mode", &u8_value) == ESP_OK) {
        out_settings->brightness_mode = u8_value == (uint8_t)EPHOTO_BRIGHTNESS_MODE_AUTO
                                            ? EPHOTO_BRIGHTNESS_MODE_AUTO
                                            : EPHOTO_BRIGHTNESS_MODE_MANUAL;
    }
    if (nvs_get_u8(handle, "clock_format", &u8_value) == ESP_OK) {
        out_settings->clock_format = u8_value == (uint8_t)EPHOTO_CLOCK_FORMAT_12H
                                         ? EPHOTO_CLOCK_FORMAT_12H
                                         : EPHOTO_CLOCK_FORMAT_24H;
    }
    if (nvs_get_u8(handle, "clock_pos", &u8_value) == ESP_OK) {
        out_settings->clock_position = u8_value <= (uint8_t)EPHOTO_CLOCK_POSITION_BOTTOM_RIGHT
                                           ? (ephoto_clock_position_t)u8_value
                                           : EPHOTO_CLOCK_POSITION_TOP_RIGHT;
    }
    if (nvs_get_u8(handle, "clock_color", &u8_value) == ESP_OK) {
        out_settings->clock_color = u8_value == (uint8_t)EPHOTO_CLOCK_COLOR_DARK_GRAY
                                        ? EPHOTO_CLOCK_COLOR_DARK_GRAY
                                        : EPHOTO_CLOCK_COLOR_WHITE;
    }
    if (nvs_get_u8(handle, "ota_interval", &u8_value) == ESP_OK) {
        out_settings->ota_interval = u8_value <= (uint8_t)EPHOTO_OTA_INTERVAL_1W
                                         ? (ephoto_ota_interval_t)u8_value
                                         : EPHOTO_OTA_INTERVAL_OFF;
        if (out_settings->ota_interval == EPHOTO_OTA_INTERVAL_6H) {
            out_settings->ota_interval = EPHOTO_OTA_INTERVAL_24H;
        }
    }
    if (nvs_get_u8(handle, "fit", &u8_value) == ESP_OK) {
        out_settings->fit_mode = (ephoto_fit_mode_t)u8_value;
    }
    if (nvs_get_u8(handle, "orient", &u8_value) == ESP_OK) {
        out_settings->orientation_filter = (ephoto_orientation_filter_t)u8_value;
    }
    if (nvs_get_u8(handle, "clock", &u8_value) == ESP_OK) {
        out_settings->clock_visible = u8_value != 0;
    }
    if (nvs_get_u8(handle, "screen", &u8_value) == ESP_OK) {
        out_settings->screen_on = u8_value != 0;
    }
    if (nvs_get_u8(handle, "screen_sched", &u8_value) == ESP_OK) {
        out_settings->screen_schedule_enabled = u8_value != 0;
    }
    if (nvs_get_u8(handle, "auto_rot", &u8_value) == ESP_OK) {
        out_settings->auto_rotation_enabled = u8_value != 0;
    }
    if (nvs_get_u8(handle, "auto_tsync", &u8_value) == ESP_OK) {
        out_settings->auto_time_sync = u8_value != 0;
    }
    if (nvs_get_u16(handle, "screen_on_m", &u16_value) == ESP_OK) {
        out_settings->screen_on_minute = u16_value < 1440U ? u16_value : out_settings->screen_on_minute;
    }
    if (nvs_get_u16(handle, "screen_off_m", &u16_value) == ESP_OK) {
        out_settings->screen_off_minute = u16_value < 1440U ? u16_value : out_settings->screen_off_minute;
    }
    if (nvs_get_u16(handle, "rotate", &u16_value) == ESP_OK) {
        out_settings->rotation_deg = u16_value == 90 ? 90 : 0;
        out_settings->manual_rotation_deg = out_settings->rotation_deg;
    }
    if (nvs_get_u16(handle, "manual_rot", &u16_value) == ESP_OK) {
        out_settings->manual_rotation_deg = u16_value == 90 ? 90 : 0;
    }
    if (!out_settings->auto_rotation_enabled) {
        out_settings->rotation_deg = out_settings->manual_rotation_deg;
    }
    if (nvs_get_str(handle, "tz", out_settings->timezone, &tz_len) != ESP_OK ||
        out_settings->timezone[0] == '\0') {
        strlcpy(out_settings->timezone, s_cached_settings.timezone, sizeof(out_settings->timezone));
    }
    size_t ota_channel_len = sizeof(out_settings->ota_channel);
    if (nvs_get_str(handle, "ota_chan", out_settings->ota_channel, &ota_channel_len) == ESP_OK) {
        if (strcmp(out_settings->ota_channel, "stable") != 0) {
            strlcpy(out_settings->ota_channel, "stable", sizeof(out_settings->ota_channel));
        }
    } else {
        strlcpy(out_settings->ota_channel, s_cached_settings.ota_channel, sizeof(out_settings->ota_channel));
    }
    ESP_LOGI(TAG, "loaded settings: ota_channel=%s ota_interval=%d", out_settings->ota_channel, (int)out_settings->ota_interval);

    s_cached_settings = *out_settings;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t settings_service_save(const ephoto_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "open nvs failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "mode", settings->playback_mode), done, TAG, "save mode failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "interval", settings->slideshow_interval), done, TAG, "save interval failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "bright_pct", settings->manual_brightness), done, TAG, "save brightness failed");
    nvs_erase_key(handle, "bright");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "bright_mode", settings->brightness_mode), done, TAG, "save brightness mode failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "clock_format", settings->clock_format), done, TAG, "save clock format failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "clock_pos", settings->clock_position), done, TAG, "save clock position failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "clock_color", settings->clock_color), done, TAG, "save clock color failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "ota_interval", settings->ota_interval), done, TAG, "save ota interval failed");
    nvs_erase_key(handle, "clock_style");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "fit", settings->fit_mode), done, TAG, "save fit failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "orient", settings->orientation_filter), done, TAG, "save orientation failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "clock", settings->clock_visible), done, TAG, "save clock failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "screen", settings->screen_on), done, TAG, "save screen failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "screen_sched", settings->screen_schedule_enabled), done, TAG, "save screen schedule failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "auto_rot", settings->auto_rotation_enabled), done, TAG, "save auto rotation failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "auto_tsync", settings->auto_time_sync), done, TAG, "save auto time sync failed");
    ESP_GOTO_ON_ERROR(nvs_set_u16(handle, "screen_on_m", settings->screen_on_minute), done, TAG, "save screen on time failed");
    ESP_GOTO_ON_ERROR(nvs_set_u16(handle, "screen_off_m", settings->screen_off_minute), done, TAG, "save screen off time failed");
    ESP_GOTO_ON_ERROR(nvs_set_u16(handle, "rotate", settings->manual_rotation_deg), done, TAG, "save rotation failed");
    ESP_GOTO_ON_ERROR(nvs_set_u16(handle, "manual_rot", settings->manual_rotation_deg), done, TAG, "save manual rotation failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, "tz", settings->timezone), done, TAG, "save tz failed");
    nvs_erase_key(handle, "ota_srv_url");
    nvs_erase_key(handle, "ota_url");
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, "ota_chan", settings->ota_channel), done, TAG, "save ota channel failed");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), done, TAG, "commit failed");
    ESP_LOGI(TAG, "saved settings: ota_channel=%s ota_interval=%d", settings->ota_channel, (int)settings->ota_interval);
    s_cached_settings = *settings;
done:
    nvs_close(handle);
    return ret;
}
