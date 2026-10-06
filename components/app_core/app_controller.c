#include "app_controller.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ambient_light_service.h"
#include "boot_image_service.h"
#include "clock_service.h"
#include "display_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gallery_service.h"
#include "hosted_service.h"
#include "notification_service.h"
#include "ota_service.h"
#include "settings_service.h"
#include "storage_service.h"
#include "wifi_admin.h"

static const char *TAG = "app_controller";
static const board_profile_t *s_profile;
static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_state_mutex;
static ephoto_app_state_t s_state;
static bool s_settings_dirty;
static int64_t s_settings_dirty_at_ms;
static bool s_media_rescan_requested;
static bool s_cache_build_requested;
static bool s_cache_build_request_locks_interaction;
static bool s_cache_build_request_upload_batch;
static bool s_focus_latest_uploaded_after_cache;
static char s_pending_uploaded_photo_path[EPHOTO_MAX_PHOTO_PATH_LEN];
static size_t s_pending_uploaded_photo_count;
static char s_pending_uploaded_photo_paths[32][EPHOTO_MAX_PHOTO_PATH_LEN];
static int s_pending_selected_photo_index = -1;
static int s_pending_rebuild_photo_index = -1;
static ephoto_settings_t s_menu_draft_settings;
static ephoto_settings_t s_menu_original_settings;
static uint8_t s_menu_edit_choice_index;
static bool s_album_visible;
static bool s_album_action_popup_visible;
static uint8_t s_album_action_index;
static bool s_album_action_confirm_pending;
static uint8_t s_album_action_confirm_index;
static int s_album_selected_index = -1;
static int32_t s_album_slot_photo_indices[EPHOTO_ALBUM_MAX_SLOTS];
static bool s_album_slot_thumb_ready[EPHOTO_ALBUM_MAX_SLOTS];
static TaskHandle_t s_control_task_handle;
static TaskHandle_t s_media_task_handle;
static TaskHandle_t s_photo_task_handle;
static TaskHandle_t s_ui_task_handle;
static TaskHandle_t s_settings_task_handle;

#define EPHOTO_CORE_INTERACTIVE 0
#define EPHOTO_CORE_BACKGROUND  1
#define EPHOTO_PRIO_INPUT       12
#define EPHOTO_PRIO_CONTROL     11
#define EPHOTO_PRIO_UI          10
#define EPHOTO_PRIO_HTTPD       9
#define EPHOTO_PRIO_PHOTO       6
#define EPHOTO_PRIO_MEDIA       5
#define EPHOTO_PRIO_SETTINGS    4
// Photo delete/rotate commands synchronously traverse gallery, cache, and
// notification paths. 8 KiB is insufficient once newlib formatting is active.
#define EPHOTO_STACK_CONTROL    16384
#define EPHOTO_STACK_MEDIA      10240
#define EPHOTO_STACK_PHOTO      12288
#define EPHOTO_STACK_UI         12288
#define EPHOTO_STACK_SETTINGS   8192
#define EPHOTO_MENU_OPTION_COUNT 10
#define EPHOTO_MENU_TOTAL_ITEMS EPHOTO_MENU_OPTION_COUNT

typedef enum {
    EPHOTO_CACHE_BUILD_NONE = 0,
    EPHOTO_CACHE_BUILD_FULL,
    EPHOTO_CACHE_BUILD_UPLOAD_BATCH,
    EPHOTO_CACHE_BUILD_REBUILD_SINGLE,
} ephoto_cache_build_mode_t;

typedef struct {
    uint32_t base_done;
    uint32_t base_failed;
    uint32_t total;
    uint32_t photo_total;
    uint32_t photo_index;
    bool lock_interaction;
    char photo_name[EPHOTO_MAX_PHOTO_NAME_LEN];
} cache_progress_context_t;

static ephoto_cache_build_mode_t s_cache_build_mode;

typedef enum {
    EPHOTO_MENU_ITEM_ALBUM = 0,
    EPHOTO_MENU_ITEM_PLAYBACK,
    EPHOTO_MENU_ITEM_INTERVAL,
    EPHOTO_MENU_ITEM_FIT_MODE,
    EPHOTO_MENU_ITEM_FILTER,
    EPHOTO_MENU_ITEM_ROTATION,
    EPHOTO_MENU_ITEM_AUTO_ROTATION,
    EPHOTO_MENU_ITEM_BRIGHTNESS,
    EPHOTO_MENU_ITEM_CLOCK,
    EPHOTO_MENU_ITEM_CLOCK_FORMAT,
} ephoto_menu_item_id_t;

static uint8_t s_menu_selected_index;

static void clear_pending_selected_photo_locked(void);
static void clear_pending_rebuild_photo_locked(void);
static void set_notification_locked(ephoto_notification_level_t level, const char *text, const char *screen_text);
static void set_cache_build_state_locked(bool active,
                                         bool lock_interaction,
                                         uint32_t total,
                                         uint32_t done,
                                         uint32_t failed,
                                         uint32_t photo_total,
                                         uint32_t photo_index,
                                         const char *photo_name);
static void sync_menu_snapshot_locked(void);
static void album_sync_snapshot_locked(void);
static bool refresh_effective_rotation_locked(void);

static ephoto_app_state_t *allocate_task_snapshot(const char *task_name)
{
    ephoto_app_state_t *snapshot =
        heap_caps_calloc(1, sizeof(ephoto_app_state_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        snapshot = calloc(1, sizeof(ephoto_app_state_t));
    }
    if (!snapshot) {
        ESP_LOGE(TAG, "%s snapshot allocation failed", task_name ? task_name : "task");
    }
    return snapshot;
}

static void sync_storage_locked(void)
{
    storage_service_refresh_stats();
    storage_service_get_status(&s_state.storage);
    s_state.storage.photo_count = gallery_service_get_count_filtered(s_state.settings.orientation_filter);
}

static void sync_network_locked(void)
{
    s_state.network_mode = wifi_admin_get_mode();
    strlcpy(s_state.current_ssid, wifi_admin_get_current_ssid(), sizeof(s_state.current_ssid));
}

static void sync_hosted_locked(void)
{
    hosted_service_get_status(&s_state.hosted);
}

static void sync_boot_image_locked(void)
{
    boot_image_service_get_status(&s_state.boot_image);
}

static bool sync_ota_locked(void)
{
    ephoto_ota_status_t latest = {0};
    ota_service_get_status(&latest);
    if (memcmp(&s_state.ota, &latest, sizeof(latest)) == 0) {
        return false;
    }
    s_state.ota = latest;
    return true;
}

static void notify_ota_transition_locked(const ephoto_ota_status_t *prev_status,
                                         const ephoto_ota_status_t *next_status)
{
    if (!next_status) {
        return;
    }

    if (next_status->stage == EPHOTO_OTA_STAGE_UPDATE_AVAILABLE &&
        (!prev_status || prev_status->stage != EPHOTO_OTA_STAGE_UPDATE_AVAILABLE)) {
        char message[EPHOTO_MAX_NOTIFICATION_LEN];
        snprintf(message,
                 sizeof(message),
                 "发现新版本 %s，可在设置中立即升级",
                 next_status->available_version[0] ? next_status->available_version : "未知版本");
        set_notification_locked(EPHOTO_NOTIFICATION_INFO, message, "发现更新");
        return;
    }

    if (next_status->stage == EPHOTO_OTA_STAGE_UP_TO_DATE &&
        next_status->last_manual_trigger &&
        (!prev_status || prev_status->stage == EPHOTO_OTA_STAGE_CHECKING)) {
        set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "当前已是最新版本", "已是最新");
        return;
    }

    if (next_status->stage == EPHOTO_OTA_STAGE_RESTARTING &&
        (!prev_status || prev_status->stage != EPHOTO_OTA_STAGE_RESTARTING ||
         strcmp(prev_status->message, next_status->message) != 0)) {
        set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS,
                                next_status->message[0] ? next_status->message : "设备即将更新，重启过程中请勿断开电源",
                                next_status->message[0] ? next_status->message : "设备即将更新，重启过程中请勿断开电源");
        return;
    }

    if (next_status->stage == EPHOTO_OTA_STAGE_SUCCESS &&
        (!prev_status || prev_status->stage != EPHOTO_OTA_STAGE_SUCCESS)) {
        set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS,
                                next_status->message[0] ? next_status->message : "固件更新成功",
                                "更新成功");
        return;
    }

    if (next_status->stage == EPHOTO_OTA_STAGE_ERROR &&
        (!prev_status || prev_status->stage != EPHOTO_OTA_STAGE_ERROR ||
         strcmp(prev_status->message, next_status->message) != 0)) {
        set_notification_locked(EPHOTO_NOTIFICATION_ERROR,
                                next_status->message[0] ? next_status->message : "远程更新失败",
                                "更新失败");
    }
}

static void sync_current_photo_locked(void)
{
    ephoto_photo_t photo;
    if (gallery_service_get_item(s_state.current_photo_index, &photo) == ESP_OK) {
        strlcpy(s_state.current_photo_name, photo.name, sizeof(s_state.current_photo_name));
        strlcpy(s_state.current_photo_path, photo.path, sizeof(s_state.current_photo_path));
    } else {
        s_state.current_photo_index = -1;
        s_state.current_photo_name[0] = '\0';
        s_state.current_photo_path[0] = '\0';
    }
}

static void prime_startup_photo_if_possible(void)
{
    if (!s_state.current_photo_path[0]) {
        return;
    }

    bool cache_exists = display_service_has_cache(s_state.current_photo_path,
                                                  s_state.settings.fit_mode,
                                                  s_state.settings.rotation_deg);
    if (!cache_exists) {
        ESP_LOGI(TAG,
                 "startup skip synchronous cache prepare for %s, wait for background photo task",
                 s_state.current_photo_path);
        return;
    }

    esp_err_t stage_err = display_service_stage_photo(s_state.current_photo_path,
                                                      s_state.settings.fit_mode,
                                                      s_state.settings.rotation_deg);
    if (stage_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "startup stage current photo failed for %s: %s",
                 s_state.current_photo_path,
                 esp_err_to_name(stage_err));
    }
}

static bool is_photo_deferred_locked(const ephoto_photo_t *photo)
{
    return display_service_should_defer_photo(photo,
                                              s_state.settings.fit_mode,
                                              s_state.settings.rotation_deg);
}

static bool photo_matches_orientation_filter(const ephoto_photo_t *photo,
                                             ephoto_orientation_filter_t filter)
{
    if (!photo) {
        return false;
    }

    if (filter == EPHOTO_ORIENTATION_ALL) {
        return true;
    }

    if (photo->width == 0 || photo->height == 0) {
        return true;
    }

    bool landscape = photo->width >= photo->height;
    if (filter == EPHOTO_ORIENTATION_LANDSCAPE) {
        return landscape;
    }
    if (filter == EPHOTO_ORIENTATION_PORTRAIT) {
        return !landscape;
    }
    return true;
}

static const char *rotation_label_key(uint16_t rotation_deg)
{
    return rotation_deg == 90 ? "横向" : "纵向";
}

static ephoto_brightness_t normalize_brightness(ephoto_brightness_t brightness)
{
    if (brightness > EPHOTO_BRIGHTNESS_MAX) {
        return EPHOTO_BRIGHTNESS_MAX;
    }
    return brightness;
}

static ephoto_brightness_t brightness_from_i32(int32_t value)
{
    if (value <= 0) {
        return EPHOTO_BRIGHTNESS_MIN;
    }
    if (value >= EPHOTO_BRIGHTNESS_MAX) {
        return EPHOTO_BRIGHTNESS_MAX;
    }
    return (ephoto_brightness_t)value;
}

static uint16_t normalize_minute_of_day(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value >= 1440) {
        return 1439;
    }
    return (uint16_t)value;
}

static ephoto_brightness_t auto_brightness_from_percent(uint8_t percent)
{
    const ephoto_brightness_t min_brightness = 5;
    uint32_t clamped = percent > 100U ? 100U : percent;
    return (ephoto_brightness_t)(min_brightness +
                                 ((EPHOTO_BRIGHTNESS_MAX - min_brightness) * clamped + 50U) / 100U);
}

static uint16_t rotation_from_switch_level(int level)
{
    bool active = s_profile && s_profile->rotation_switch_active_low ? (level == 0) : (level != 0);
    return active ? 0U : 90U;
}

static bool manual_rotation_blocked_by_auto_locked(void)
{
    return s_state.settings.auto_rotation_enabled;
}

static uint16_t resolve_effective_rotation_deg_locked(const ephoto_settings_t *settings)
{
    if (!settings) {
        return 0;
    }
    if (!settings->auto_rotation_enabled ||
        !s_profile ||
        s_profile->rotation_switch_gpio == GPIO_NUM_NC) {
        return settings->manual_rotation_deg == 90 ? 90U : 0U;
    }

    int level = gpio_get_level(s_profile->rotation_switch_gpio);
    return rotation_from_switch_level(level);
}

static bool schedule_should_turn_screen_on(const ephoto_settings_t *settings, const struct tm *tm_now)
{
    if (!settings || !tm_now || !settings->screen_schedule_enabled) {
        return true;
    }

    uint16_t on_minute = settings->screen_on_minute < 1440U ? settings->screen_on_minute : 0U;
    uint16_t off_minute = settings->screen_off_minute < 1440U ? settings->screen_off_minute : 0U;
    uint16_t now_minute = (uint16_t)(tm_now->tm_hour * 60 + tm_now->tm_min);

    if (on_minute == off_minute) {
        return true;
    }
    if (off_minute < on_minute) {
        return !(now_minute >= off_minute && now_minute < on_minute);
    }
    return !(now_minute >= off_minute || now_minute < on_minute);
}

static void sync_ambient_light_locked(const ambient_light_status_t *status)
{
    if (!status || !status->available || !status->valid) {
        s_state.ambient_light_raw_mv = 0;
        s_state.ambient_light_percent = 0;
        return;
    }

    s_state.ambient_light_raw_mv = status->millivolts;
    s_state.ambient_light_percent = status->percent;
}

static void set_manual_brightness_locked(ephoto_brightness_t brightness)
{
    ephoto_brightness_t normalized = normalize_brightness(brightness);
    s_state.settings.brightness_mode = EPHOTO_BRIGHTNESS_MODE_MANUAL;
    s_state.settings.manual_brightness = normalized;
    s_state.settings.brightness = normalized;
}

static bool refresh_effective_rotation_locked(void)
{
    uint16_t resolved = resolve_effective_rotation_deg_locked(&s_state.settings);
    if (s_state.settings.rotation_deg == resolved) {
        return false;
    }
    s_state.settings.rotation_deg = resolved;
    return true;
}

static ephoto_brightness_t brightness_preset_value(uint8_t preset_index)
{
    switch (preset_index) {
    case 0:
        return EPHOTO_BRIGHTNESS_PRESET_LOW;
    case 1:
        return EPHOTO_BRIGHTNESS_PRESET_MEDIUM;
    default:
        return EPHOTO_BRIGHTNESS_PRESET_HIGH;
    }
}

static uint8_t brightness_preset_index(ephoto_brightness_t brightness)
{
    brightness = normalize_brightness(brightness);
    if (brightness <= (EPHOTO_BRIGHTNESS_PRESET_LOW + EPHOTO_BRIGHTNESS_PRESET_MEDIUM) / 2) {
        return 0;
    }
    if (brightness <= (EPHOTO_BRIGHTNESS_PRESET_MEDIUM + EPHOTO_BRIGHTNESS_PRESET_HIGH) / 2) {
        return 1;
    }
    return 2;
}

static ephoto_brightness_t next_brightness_level(ephoto_brightness_t brightness)
{
    return brightness_preset_value((uint8_t)((brightness_preset_index(brightness) + 1) % 3));
}

static const char *brightness_preset_label_key(uint8_t preset_index)
{
    switch (preset_index) {
    case 0:
        return "25%";
    case 1:
        return "60%";
    default:
        return "100%";
    }
}

static void brightness_value_string(ephoto_brightness_t brightness, char *out_value, size_t out_value_len)
{
    if (!out_value || out_value_len == 0) {
        return;
    }

    snprintf(out_value, out_value_len, "%u%%", (unsigned)normalize_brightness(brightness));
}

static const char *clock_format_label_key(ephoto_clock_format_t format)
{
    return format == EPHOTO_CLOCK_FORMAT_12H ? "12小时" : "24小时";
}

static ephoto_clock_position_t normalize_clock_position(ephoto_clock_position_t position)
{
    switch (position) {
    case EPHOTO_CLOCK_POSITION_TOP_LEFT:
    case EPHOTO_CLOCK_POSITION_BOTTOM_LEFT:
    case EPHOTO_CLOCK_POSITION_BOTTOM_RIGHT:
    case EPHOTO_CLOCK_POSITION_TOP_RIGHT:
        return position;
    default:
        return EPHOTO_CLOCK_POSITION_TOP_RIGHT;
    }
}

static ephoto_clock_color_t normalize_clock_color(ephoto_clock_color_t color)
{
    return color == EPHOTO_CLOCK_COLOR_DARK_GRAY
               ? EPHOTO_CLOCK_COLOR_DARK_GRAY
               : EPHOTO_CLOCK_COLOR_WHITE;
}

static bool settings_equal(const ephoto_settings_t *lhs, const ephoto_settings_t *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }

    return lhs->playback_mode == rhs->playback_mode &&
           lhs->slideshow_interval == rhs->slideshow_interval &&
           lhs->brightness == rhs->brightness &&
           lhs->manual_brightness == rhs->manual_brightness &&
           lhs->brightness_mode == rhs->brightness_mode &&
           lhs->clock_format == rhs->clock_format &&
           lhs->clock_position == rhs->clock_position &&
           lhs->clock_color == rhs->clock_color &&
           lhs->ota_interval == rhs->ota_interval &&
           lhs->fit_mode == rhs->fit_mode &&
           lhs->orientation_filter == rhs->orientation_filter &&
           lhs->clock_visible == rhs->clock_visible &&
           lhs->screen_on == rhs->screen_on &&
           lhs->screen_schedule_enabled == rhs->screen_schedule_enabled &&
           lhs->auto_rotation_enabled == rhs->auto_rotation_enabled &&
           lhs->auto_time_sync == rhs->auto_time_sync &&
           lhs->screen_on_minute == rhs->screen_on_minute &&
           lhs->screen_off_minute == rhs->screen_off_minute &&
           lhs->rotation_deg == rhs->rotation_deg &&
           lhs->manual_rotation_deg == rhs->manual_rotation_deg &&
           strcmp(lhs->timezone, rhs->timezone) == 0 &&
           strcmp(lhs->ota_channel, rhs->ota_channel) == 0;
}

static bool menu_index_is_option(size_t index)
{
    return index < EPHOTO_MENU_TOTAL_ITEMS;
}

static const char *playback_mode_label_key(ephoto_playback_mode_t mode)
{
    switch (mode) {
    case EPHOTO_PLAYBACK_RANDOM:
        return "随机播放";
    case EPHOTO_PLAYBACK_TIME_DESC:
        return "时间倒序";
    case EPHOTO_PLAYBACK_TIME_ASC:
    default:
        return "时间顺序";
    }
}

static const char *interval_label_key(ephoto_slideshow_interval_t interval)
{
    switch (interval) {
    case EPHOTO_INTERVAL_OFF:
        return "不轮播";
    case EPHOTO_INTERVAL_10S:
        return "10秒";
    case EPHOTO_INTERVAL_1M:
        return "1分钟";
    case EPHOTO_INTERVAL_30M:
        return "30分钟";
    case EPHOTO_INTERVAL_60M:
        return "60分钟";
    case EPHOTO_INTERVAL_6H:
        return "6小时";
    case EPHOTO_INTERVAL_12H:
        return "12小时";
    case EPHOTO_INTERVAL_24H:
        return "24小时";
    case EPHOTO_INTERVAL_1W:
        return "1周";
    case EPHOTO_INTERVAL_1MO:
        return "1月";
    default:
        return "未知";
    }
}

static const char *fit_mode_label_key(ephoto_fit_mode_t mode)
{
    return mode == EPHOTO_FIT_COVER ? "裁切铺满" : "完整显示";
}

static const char *orientation_filter_label_key(ephoto_orientation_filter_t filter)
{
    switch (filter) {
    case EPHOTO_ORIENTATION_LANDSCAPE:
        return "只看横图";
    case EPHOTO_ORIENTATION_PORTRAIT:
        return "只看竖图";
    case EPHOTO_ORIENTATION_ALL:
    default:
        return "全部显示";
    }
}

static void photo_meta_label(const ephoto_photo_t *photo, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!photo) {
        strlcpy(out, "-", out_len);
        return;
    }
    snprintf(out,
             out_len,
             "%s %ux%u%s%s",
             photo->height > photo->width ? "竖图" : "横图",
             photo->width,
             photo->height,
             photo->manual_rotation_deg > 0 ? " 手" : (photo->effective_rotation_deg > 0 ? " 自" : ""),
             photo->effective_rotation_deg > 0
                 ? (photo->manual_rotation_deg > 0
                        ? (photo->manual_rotation_deg == 90 ? "90" :
                           photo->manual_rotation_deg == 180 ? "180" : "270")
                        : (photo->effective_rotation_deg == 90 ? "90" :
                           photo->effective_rotation_deg == 180 ? "180" : "270"))
                 : "");
}

static bool photo_supports_orientation_adjustment(const ephoto_photo_t *photo)
{
    const char *ext = NULL;
    if (!photo || !photo->path[0]) {
        return false;
    }
    ext = strrchr(photo->path, '.');
    if (!ext) {
        return false;
    }
    return strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0;
}

static bool album_action_rotation_target(uint8_t action_index, uint16_t *out_rotation_deg)
{
    if (!out_rotation_deg) {
        return false;
    }

    switch (action_index) {
    case 1:
        *out_rotation_deg = 90;
        return true;
    case 2:
        *out_rotation_deg = 180;
        return true;
    case 3:
        *out_rotation_deg = 270;
        return true;
    case 4:
        *out_rotation_deg = 0;
        return true;
    default:
        return false;
    }
}

static bool album_action_needs_rebuild_confirmation(uint8_t action_index)
{
    return action_index >= 1 && action_index <= 4;
}

static const char *photo_rotation_action_title(uint16_t rotation_deg)
{
    switch (rotation_deg) {
    case 90:
        return "旋转90度";
    case 180:
        return "旋转180度";
    case 270:
        return "旋转270度";
    case 0:
    default:
        return "恢复方向";
    }
}

static const char *photo_orientation_label(const ephoto_photo_t *photo)
{
    if (!photo) {
        return "未知方向";
    }
    return photo->height > photo->width ? "竖图" : "横图";
}

static const char *orientation_filter_mode_label(ephoto_orientation_filter_t filter)
{
    switch (filter) {
    case EPHOTO_ORIENTATION_LANDSCAPE:
        return "只看横图";
    case EPHOTO_ORIENTATION_PORTRAIT:
        return "只看竖图";
    case EPHOTO_ORIENTATION_ALL:
    default:
        return "全部显示";
    }
}

static void orientation_filter_block_message(const ephoto_photo_t *photo,
                                             ephoto_orientation_filter_t filter,
                                             char *out,
                                             size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    switch (filter) {
    case EPHOTO_ORIENTATION_LANDSCAPE:
        snprintf(out,
                 out_len,
                 "当前是%s，这张照片是%s，不能显示",
                 orientation_filter_mode_label(filter),
                 photo_orientation_label(photo));
        break;
    case EPHOTO_ORIENTATION_PORTRAIT:
        snprintf(out,
                 out_len,
                 "当前是%s，这张照片是%s，不能显示",
                 orientation_filter_mode_label(filter),
                 photo_orientation_label(photo));
        break;
    case EPHOTO_ORIENTATION_ALL:
    default:
        strlcpy(out, "当前显示设置不允许这张照片显示", out_len);
        break;
    }
}

static const char *menu_item_label(size_t index)
{
    switch (index) {
    case EPHOTO_MENU_ITEM_ALBUM:
        return "相册";
    case EPHOTO_MENU_ITEM_PLAYBACK:
        return "播放顺序";
    case EPHOTO_MENU_ITEM_INTERVAL:
        return "轮播间隔";
    case EPHOTO_MENU_ITEM_FIT_MODE:
        return "显示比例";
    case EPHOTO_MENU_ITEM_FILTER:
        return "横竖选择";
    case EPHOTO_MENU_ITEM_ROTATION:
        return "旋转屏幕";
    case EPHOTO_MENU_ITEM_AUTO_ROTATION:
        return "自动旋转";
    case EPHOTO_MENU_ITEM_BRIGHTNESS:
        return "亮度调节";
    case EPHOTO_MENU_ITEM_CLOCK:
        return "时钟显示";
    case EPHOTO_MENU_ITEM_CLOCK_FORMAT:
        return "时间格式";
    default:
        return "";
    }
}

static void menu_item_value_string_locked(size_t index, char *out_value, size_t out_value_len)
{
    if (!out_value || out_value_len == 0) {
        return;
    }

    out_value[0] = '\0';

    switch (index) {
    case EPHOTO_MENU_ITEM_ALBUM:
        strlcpy(out_value, "进入", out_value_len);
        break;
    case EPHOTO_MENU_ITEM_PLAYBACK:
        strlcpy(out_value, playback_mode_label_key(s_menu_draft_settings.playback_mode), out_value_len);
        break;
    case EPHOTO_MENU_ITEM_INTERVAL:
        strlcpy(out_value, interval_label_key(s_menu_draft_settings.slideshow_interval), out_value_len);
        break;
    case EPHOTO_MENU_ITEM_FIT_MODE:
        strlcpy(out_value, fit_mode_label_key(s_menu_draft_settings.fit_mode), out_value_len);
        break;
    case EPHOTO_MENU_ITEM_FILTER:
        strlcpy(out_value, orientation_filter_label_key(s_menu_draft_settings.orientation_filter), out_value_len);
        break;
    case EPHOTO_MENU_ITEM_ROTATION:
        strlcpy(out_value, rotation_label_key(s_menu_draft_settings.manual_rotation_deg), out_value_len);
        break;
    case EPHOTO_MENU_ITEM_AUTO_ROTATION:
        strlcpy(out_value, s_menu_draft_settings.auto_rotation_enabled ? "开启" : "关闭", out_value_len);
        break;
    case EPHOTO_MENU_ITEM_BRIGHTNESS:
        brightness_value_string(s_menu_draft_settings.brightness, out_value, out_value_len);
        break;
    case EPHOTO_MENU_ITEM_CLOCK:
        strlcpy(out_value, s_menu_draft_settings.clock_visible ? "开启" : "关闭", out_value_len);
        break;
    case EPHOTO_MENU_ITEM_CLOCK_FORMAT:
        strlcpy(out_value, clock_format_label_key(s_menu_draft_settings.clock_format), out_value_len);
        break;
    default:
        break;
    }
}

static uint8_t menu_choice_count(size_t index)
{
    switch (index) {
    case EPHOTO_MENU_ITEM_ALBUM:
        return 0;
    case EPHOTO_MENU_ITEM_PLAYBACK:
        return 3;
    case EPHOTO_MENU_ITEM_INTERVAL:
        return 10;
    case EPHOTO_MENU_ITEM_FIT_MODE:
        return 2;
    case EPHOTO_MENU_ITEM_FILTER:
        return 3;
    case EPHOTO_MENU_ITEM_ROTATION:
        return 2;
    case EPHOTO_MENU_ITEM_AUTO_ROTATION:
        return 2;
    case EPHOTO_MENU_ITEM_BRIGHTNESS:
        return 3;
    case EPHOTO_MENU_ITEM_CLOCK:
        return 2;
    case EPHOTO_MENU_ITEM_CLOCK_FORMAT:
        return 2;
    default:
        return 0;
    }
}

static void menu_choice_string(size_t menu_index,
                               uint8_t choice_index,
                               char *out_value,
                               size_t out_value_len)
{
    if (!out_value || out_value_len == 0) {
        return;
    }

    out_value[0] = '\0';

    switch (menu_index) {
    case EPHOTO_MENU_ITEM_PLAYBACK:
    {
        static const ephoto_playback_mode_t modes[] = {
            EPHOTO_PLAYBACK_TIME_ASC,
            EPHOTO_PLAYBACK_TIME_DESC,
            EPHOTO_PLAYBACK_RANDOM,
        };
        if (choice_index < sizeof(modes) / sizeof(modes[0])) {
            strlcpy(out_value, playback_mode_label_key(modes[choice_index]), out_value_len);
        }
        break;
    }
    case EPHOTO_MENU_ITEM_INTERVAL:
        strlcpy(out_value, interval_label_key((ephoto_slideshow_interval_t)choice_index), out_value_len);
        break;
    case EPHOTO_MENU_ITEM_FIT_MODE:
        strlcpy(out_value, choice_index == 0 ? "完整显示" : "裁切铺满", out_value_len);
        break;
    case EPHOTO_MENU_ITEM_FILTER:
        strlcpy(out_value,
                orientation_filter_label_key((ephoto_orientation_filter_t)choice_index),
                out_value_len);
        break;
    case EPHOTO_MENU_ITEM_ROTATION:
        strlcpy(out_value, choice_index == 0 ? "纵向" : "横向", out_value_len);
        break;
    case EPHOTO_MENU_ITEM_AUTO_ROTATION:
        strlcpy(out_value, choice_index == 0 ? "关闭" : "开启", out_value_len);
        break;
    case EPHOTO_MENU_ITEM_BRIGHTNESS:
        strlcpy(out_value, brightness_preset_label_key(choice_index), out_value_len);
        break;
    case EPHOTO_MENU_ITEM_CLOCK:
        strlcpy(out_value, choice_index == 0 ? "关闭" : "开启", out_value_len);
        break;
    case EPHOTO_MENU_ITEM_CLOCK_FORMAT:
        strlcpy(out_value, choice_index == 0 ? "24小时" : "12小时", out_value_len);
        break;
    default:
        break;
    }
}

static uint8_t menu_current_choice_index_locked(size_t menu_index)
{
    switch (menu_index) {
    case EPHOTO_MENU_ITEM_PLAYBACK:
        return (uint8_t)s_menu_draft_settings.playback_mode;
    case EPHOTO_MENU_ITEM_INTERVAL:
        return (uint8_t)s_menu_draft_settings.slideshow_interval;
    case EPHOTO_MENU_ITEM_FIT_MODE:
        return s_menu_draft_settings.fit_mode == EPHOTO_FIT_COVER ? 1 : 0;
    case EPHOTO_MENU_ITEM_FILTER:
        return (uint8_t)s_menu_draft_settings.orientation_filter;
    case EPHOTO_MENU_ITEM_ROTATION:
        return s_menu_draft_settings.manual_rotation_deg == 90 ? 1 : 0;
    case EPHOTO_MENU_ITEM_AUTO_ROTATION:
        return s_menu_draft_settings.auto_rotation_enabled ? 1 : 0;
    case EPHOTO_MENU_ITEM_BRIGHTNESS:
        return brightness_preset_index(s_menu_draft_settings.brightness);
    case EPHOTO_MENU_ITEM_CLOCK:
        return s_menu_draft_settings.clock_visible ? 1 : 0;
    case EPHOTO_MENU_ITEM_CLOCK_FORMAT:
        return s_menu_draft_settings.clock_format == EPHOTO_CLOCK_FORMAT_12H ? 1 : 0;
    default:
        return 0;
    }
}

static void menu_apply_choice_locked(size_t menu_index, uint8_t choice_index)
{
    switch (menu_index) {
    case EPHOTO_MENU_ITEM_PLAYBACK:
        s_menu_draft_settings.playback_mode = (ephoto_playback_mode_t)choice_index;
        break;
    case EPHOTO_MENU_ITEM_INTERVAL:
        s_menu_draft_settings.slideshow_interval = (ephoto_slideshow_interval_t)choice_index;
        break;
    case EPHOTO_MENU_ITEM_FIT_MODE:
        s_menu_draft_settings.fit_mode = choice_index == 0 ? EPHOTO_FIT_CONTAIN : EPHOTO_FIT_COVER;
        break;
    case EPHOTO_MENU_ITEM_FILTER:
        s_menu_draft_settings.orientation_filter = (ephoto_orientation_filter_t)choice_index;
        break;
    case EPHOTO_MENU_ITEM_ROTATION:
        s_menu_draft_settings.manual_rotation_deg = choice_index == 0 ? 0 : 90;
        if (!s_menu_draft_settings.auto_rotation_enabled) {
            s_menu_draft_settings.rotation_deg = s_menu_draft_settings.manual_rotation_deg;
        }
        break;
    case EPHOTO_MENU_ITEM_AUTO_ROTATION:
        s_menu_draft_settings.auto_rotation_enabled = choice_index != 0;
        break;
    case EPHOTO_MENU_ITEM_BRIGHTNESS:
        s_menu_draft_settings.brightness_mode = EPHOTO_BRIGHTNESS_MODE_MANUAL;
        s_menu_draft_settings.manual_brightness = brightness_preset_value(choice_index);
        s_menu_draft_settings.brightness = s_menu_draft_settings.manual_brightness;
        break;
    case EPHOTO_MENU_ITEM_CLOCK:
        s_menu_draft_settings.clock_visible = choice_index != 0;
        break;
    case EPHOTO_MENU_ITEM_CLOCK_FORMAT:
        s_menu_draft_settings.clock_format = choice_index == 0
                                                 ? EPHOTO_CLOCK_FORMAT_24H
                                                 : EPHOTO_CLOCK_FORMAT_12H;
        break;
    default:
        break;
    }
}

static int choose_photo_index_locked(int current_index, int delta)
{
    int available = gallery_service_get_count_filtered(s_state.settings.orientation_filter);
    if (available <= 0) {
        return -1;
    }

    int fallback_index = -1;
    int cursor = current_index;
    for (int attempts = 0; attempts < available; ++attempts) {
        cursor = gallery_service_step_index(cursor,
                                            s_state.settings.playback_mode,
                                            s_state.settings.orientation_filter,
                                            delta);
        if (cursor < 0) {
            break;
        }

        ephoto_photo_t photo = {0};
        if (gallery_service_get_item(cursor, &photo) != ESP_OK) {
            continue;
        }
        if (fallback_index < 0) {
            fallback_index = cursor;
        }
        if (!is_photo_deferred_locked(&photo)) {
            return cursor;
        }
    }

    return fallback_index;
}

static int filtered_nth_photo_index_locked(uint32_t ordinal)
{
    int cursor = -1;
    for (uint32_t i = 0; i <= ordinal; ++i) {
        cursor = gallery_service_step_index(cursor,
                                            EPHOTO_PLAYBACK_TIME_DESC,
                                            s_state.settings.orientation_filter,
                                            1);
        if (cursor < 0) {
            return -1;
        }
    }
    return cursor;
}

static int filtered_photo_position_locked(int photo_index)
{
    if (photo_index < 0) {
        return -1;
    }

    int total = gallery_service_get_count_filtered(s_state.settings.orientation_filter);
    int cursor = -1;
    for (int pos = 0; pos < total; ++pos) {
        cursor = gallery_service_step_index(cursor,
                                            EPHOTO_PLAYBACK_TIME_DESC,
                                            s_state.settings.orientation_filter,
                                            1);
        if (cursor < 0) {
            break;
        }
        if (cursor == photo_index) {
            return pos;
        }
    }
    return -1;
}

static uint8_t album_columns_locked(void)
{
    return 5U;
}

static uint8_t album_slot_capacity_locked(void)
{
    return EPHOTO_ALBUM_MAX_SLOTS;
}

static void album_action_popup_set_locked(bool visible)
{
    s_album_action_popup_visible = visible;
    if (!visible) {
        s_album_action_confirm_pending = false;
        s_album_action_confirm_index = 0;
    }
    if (visible && s_album_action_index >= EPHOTO_ALBUM_MAX_ACTIONS) {
        s_album_action_index = 0;
    }
    s_state.menu_last_input_ms = clock_service_now_ms();
    album_sync_snapshot_locked();
}

static void album_sync_snapshot_locked(void)
{
    memset(&s_state.album, 0, sizeof(s_state.album));
    s_state.album.visible = s_album_visible;
    strlcpy(s_state.album.title, "相册", sizeof(s_state.album.title));

    if (!s_album_visible) {
        return;
    }

    int total = gallery_service_get_count_filtered(s_state.settings.orientation_filter);
    if (total <= 0) {
        strlcpy(s_state.album.summary, "当前没有可浏览的照片", sizeof(s_state.album.summary));
        strlcpy(s_state.album.hint, "长按返回播放", sizeof(s_state.album.hint));
        return;
    }

    if (s_album_selected_index < 0 ||
        filtered_photo_position_locked(s_album_selected_index) < 0) {
        s_album_selected_index = s_state.current_photo_index;
        if (s_album_selected_index < 0 ||
            filtered_photo_position_locked(s_album_selected_index) < 0) {
            s_album_selected_index = filtered_nth_photo_index_locked(0);
        }
    }

    int selected_pos = filtered_photo_position_locked(s_album_selected_index);
    if (selected_pos < 0) {
        selected_pos = 0;
        s_album_selected_index = filtered_nth_photo_index_locked(0);
    }

    uint8_t slot_capacity = album_slot_capacity_locked();
    uint32_t page_start = ((uint32_t)selected_pos / slot_capacity) * slot_capacity;
    uint8_t slot_count = 0;
    int selected_slot = 0;

    for (uint8_t i = 0; i < slot_capacity; ++i) {
        uint32_t ordinal = page_start + i;
        if (ordinal >= (uint32_t)total) {
            break;
        }

        int photo_index = filtered_nth_photo_index_locked(ordinal);
        if (photo_index < 0) {
            break;
        }

        ephoto_photo_t photo = {0};
        if (gallery_service_get_item(photo_index, &photo) != ESP_OK) {
            continue;
        }

        ephoto_album_slot_t *slot = &s_state.album.slots[slot_count];
        slot->photo_index = photo_index;
        slot->width = photo.width;
        slot->height = photo.height;
        slot->is_current = photo_index == s_state.current_photo_index;
        if (s_album_slot_photo_indices[slot_count] != photo_index) {
            s_album_slot_photo_indices[slot_count] = photo_index;
            s_album_slot_thumb_ready[slot_count] = display_service_has_web_thumbnail(photo.path, EPHOTO_FIT_COVER);
        }
        slot->thumb_ready = s_album_slot_thumb_ready[slot_count];
        snprintf(slot->badge, sizeof(slot->badge), "第%lu张", (unsigned long)(ordinal + 1U));
        photo_meta_label(&photo, slot->meta, sizeof(slot->meta));
        if (photo_index == s_album_selected_index) {
            selected_slot = slot_count;
        }
        slot_count += 1U;
    }

    for (uint8_t i = slot_count; i < EPHOTO_ALBUM_MAX_SLOTS; ++i) {
        s_album_slot_photo_indices[i] = -1;
        s_album_slot_thumb_ready[i] = false;
    }

    s_state.album.slot_count = slot_count;
    s_state.album.action_popup_visible = s_album_action_popup_visible;
    s_state.album.action_confirm_pending = s_album_action_confirm_pending;
    s_state.album.selected_slot = (uint8_t)selected_slot;
    s_state.album.selected_position = (uint32_t)selected_pos + 1U;
    s_state.album.page_start_position = page_start + 1U;
    s_state.album.total_count = (uint32_t)total;
    s_state.album.action_count = 6;
    if (s_album_action_index >= s_state.album.action_count) {
        s_album_action_index = 0;
    }
    s_state.album.selected_action = s_album_action_index;
    strlcpy(s_state.album.action_title,
            s_album_action_confirm_pending ? "确认重建缓存" : "选择操作",
            sizeof(s_state.album.action_title));
    strlcpy(s_state.album.actions[0], "显示当前", sizeof(s_state.album.actions[0]));
    strlcpy(s_state.album.actions[1], "旋转90度", sizeof(s_state.album.actions[1]));
    strlcpy(s_state.album.actions[2], "旋转180度", sizeof(s_state.album.actions[2]));
    strlcpy(s_state.album.actions[3], "旋转270度", sizeof(s_state.album.actions[3]));
    strlcpy(s_state.album.actions[4], "恢复方向", sizeof(s_state.album.actions[4]));
    strlcpy(s_state.album.actions[5], "删除照片", sizeof(s_state.album.actions[5]));
    snprintf(s_state.album.summary,
             sizeof(s_state.album.summary),
             "第%lu张/共%lu张",
             (unsigned long)s_state.album.selected_position,
             (unsigned long)s_state.album.total_count);
    if (s_album_action_popup_visible && s_album_action_confirm_pending) {
        strlcpy(s_state.album.hint,
                "确认后退出相册并重建缓存 长按返回取消",
                sizeof(s_state.album.hint));
    } else if (s_album_action_popup_visible) {
        strlcpy(s_state.album.hint,
                "上下/左右切换操作 确认打开所选操作",
                sizeof(s_state.album.hint));
    } else {
        strlcpy(s_state.album.hint,
                "左右换图 上下翻页 确认打开操作 长按返回",
                sizeof(s_state.album.hint));
    }
}

static void album_open_locked(void)
{
    ESP_LOGI(TAG,
             "album_open_locked: current_photo=%d selected_before=%d",
             s_state.current_photo_index,
             s_album_selected_index);
    s_album_visible = true;
    s_album_action_popup_visible = false;
    s_album_action_index = 0;
    s_album_action_confirm_pending = false;
    s_album_action_confirm_index = 0;
    s_album_selected_index = s_state.current_photo_index;
    if (s_album_selected_index < 0) {
        s_album_selected_index = filtered_nth_photo_index_locked(0);
    }
    s_state.menu_visible = false;
    memset(&s_state.menu, 0, sizeof(s_state.menu));
    s_state.menu_index = 0;
    s_state.menu_last_input_ms = clock_service_now_ms();
    album_sync_snapshot_locked();
    set_notification_locked(EPHOTO_NOTIFICATION_INFO, "相册已打开", "相册");
}

static void album_close_locked(const char *message)
{
    ESP_LOGI(TAG,
             "album_close_locked: selected=%d action=%u message=%s",
             s_album_selected_index,
             (unsigned)s_album_action_index,
             message ? message : "");
    s_album_visible = false;
    s_album_action_popup_visible = false;
    s_album_action_index = 0;
    s_album_action_confirm_pending = false;
    s_album_action_confirm_index = 0;
    memset(&s_state.album, 0, sizeof(s_state.album));
    display_service_release_album_resources();
    if (message && message[0]) {
        set_notification_locked(EPHOTO_NOTIFICATION_INFO, message, "已退出相册");
    }
}

static void album_move_selection_locked(int position_delta)
{
    int total = gallery_service_get_count_filtered(s_state.settings.orientation_filter);
    if (total <= 0) {
        album_sync_snapshot_locked();
        return;
    }

    int selected_pos = filtered_photo_position_locked(s_album_selected_index);
    if (selected_pos < 0) {
        selected_pos = 0;
    }
    selected_pos += position_delta;
    while (selected_pos < 0) {
        selected_pos += total;
    }
    selected_pos %= total;

    int next_index = filtered_nth_photo_index_locked((uint32_t)selected_pos);
    if (next_index >= 0) {
        s_album_selected_index = next_index;
    }
    s_album_action_popup_visible = false;
    s_album_action_confirm_pending = false;
    s_album_action_confirm_index = 0;
    s_state.menu_last_input_ms = clock_service_now_ms();
    album_sync_snapshot_locked();
}

static void album_cycle_action_locked(int delta)
{
    const int action_count = s_state.album.action_count > 0 ? s_state.album.action_count : EPHOTO_ALBUM_MAX_ACTIONS;
    s_album_action_confirm_pending = false;
    s_album_action_confirm_index = 0;
    int next = (int)s_album_action_index + delta;
    while (next < 0) {
        next += action_count;
    }
    s_album_action_index = (uint8_t)(next % action_count);
    s_state.menu_last_input_ms = clock_service_now_ms();
    album_sync_snapshot_locked();
}

static esp_err_t delete_photo_index_locked(int photo_index, bool *out_current_changed)
{
    ephoto_photo_t photo = {0};
    if (gallery_service_get_item(photo_index, &photo) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    int deleted_position = filtered_photo_position_locked(photo_index);
    bool deleted_current = photo_index == s_state.current_photo_index;
    if (out_current_changed) {
        *out_current_changed = false;
    }

    display_service_invalidate_photo_cache(photo.path);
    esp_err_t err = gallery_service_delete_by_path(photo.path);
    if (err != ESP_OK) {
        return err;
    }

    gallery_service_rescan();
    sync_storage_locked();
    clear_pending_selected_photo_locked();
    clear_pending_rebuild_photo_locked();

    if (s_state.storage.photo_count > 0) {
        if (deleted_current ||
            s_state.current_photo_index < 0 ||
            gallery_service_get_item(s_state.current_photo_index, &photo) != ESP_OK) {
            s_state.current_photo_index = choose_photo_index_locked(photo_index, 1);
        }
        sync_current_photo_locked();
    } else {
        s_state.current_photo_index = -1;
        sync_current_photo_locked();
    }

    if (out_current_changed) {
        *out_current_changed = deleted_current;
    }

    if (s_album_visible) {
        int total = gallery_service_get_count_filtered(s_state.settings.orientation_filter);
        if (total <= 0) {
            album_close_locked("相册已经没有照片");
        } else {
            int next_position = deleted_position;
            if (next_position < 0) {
                next_position = 0;
            }
            if (next_position >= total) {
                next_position = total - 1;
            }
            s_album_selected_index = filtered_nth_photo_index_locked((uint32_t)next_position);
            if (s_album_selected_index < 0) {
                s_album_selected_index = s_state.current_photo_index;
            }
            s_album_action_popup_visible = false;
            s_album_action_index = 0;
            s_album_action_confirm_pending = false;
            s_album_action_confirm_index = 0;
            album_sync_snapshot_locked();
        }
    }

    if (s_state.menu_visible) {
        sync_menu_snapshot_locked();
    }
    return ESP_OK;
}

static void set_delete_notification_locked(bool deleted_current)
{
    if (deleted_current) {
        if (s_state.current_photo_index >= 0 && s_state.current_photo_path[0]) {
            set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS,
                                    "当前照片已删除，已切换到下一张照片",
                                    "已切换照片");
        } else {
            set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS,
                                    "当前照片已删除，相册已经没有照片",
                                    "相册已空");
        }
        return;
    }

    set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "照片已删除", "删除完成");
}

static esp_err_t apply_photo_orientation_adjustment_locked(int photo_index,
                                                           uint16_t manual_rotation_deg)
{
    ephoto_photo_t photo = {0};
    ephoto_photo_t updated = {0};

    if (gallery_service_get_item(photo_index, &photo) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!photo_supports_orientation_adjustment(&photo)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = manual_rotation_deg == 0
                        ? gallery_service_reset_photo_orientation_by_path(photo.path, &updated)
                        : gallery_service_set_photo_manual_rotation_by_path(photo.path,
                                                                            manual_rotation_deg,
                                                                            &updated);
    if (err != ESP_OK) {
        return err;
    }

    display_service_invalidate_photo_cache(updated.path);
    sync_storage_locked();
    clear_pending_selected_photo_locked();
    clear_pending_rebuild_photo_locked();
    s_pending_rebuild_photo_index = photo_index;
    s_cache_build_mode = EPHOTO_CACHE_BUILD_REBUILD_SINGLE;
    set_cache_build_state_locked(true, false, 6, 0, 0, 1, 1, updated.name);

    if (s_state.storage.photo_count > 0) {
        if (s_state.current_photo_index >= 0) {
            ephoto_photo_t current = {0};
            if (gallery_service_get_item(s_state.current_photo_index, &current) != ESP_OK ||
                !photo_matches_orientation_filter(&current, s_state.settings.orientation_filter)) {
                s_state.current_photo_index = choose_photo_index_locked(s_state.current_photo_index, 1);
            }
        } else {
            s_state.current_photo_index = choose_photo_index_locked(-1, 1);
        }
        sync_current_photo_locked();
    } else {
        s_state.current_photo_index = -1;
        sync_current_photo_locked();
    }

    if (s_album_visible) {
        int total = gallery_service_get_count_filtered(s_state.settings.orientation_filter);
        if (total <= 0) {
            album_close_locked("相册已经没有照片");
        } else {
            int selected_pos = filtered_photo_position_locked(photo_index);
            if (selected_pos < 0) {
                selected_pos = 0;
            }
            s_album_selected_index = filtered_nth_photo_index_locked((uint32_t)selected_pos);
            if (s_album_selected_index < 0) {
                s_album_selected_index = s_state.current_photo_index;
            }
            s_album_action_popup_visible = false;
            s_album_action_index = 0;
            s_album_action_confirm_pending = false;
            s_album_action_confirm_index = 0;
            album_sync_snapshot_locked();
        }
    }

    if (s_state.menu_visible) {
        sync_menu_snapshot_locked();
    }

    return ESP_OK;
}

static esp_err_t delete_photo_path_locked(const char *path, bool *out_current_changed)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    int photo_index = gallery_service_find_index_by_path(path);
    if (photo_index < 0) {
        if (out_current_changed) {
            *out_current_changed = false;
        }
        return ESP_ERR_NOT_FOUND;
    }
    return delete_photo_index_locked(photo_index, out_current_changed);
}

static int find_latest_photo_index_locked(void)
{
    int count = gallery_service_get_count();
    if (count <= 0) {
        return -1;
    }

    int latest_index = -1;
    time_t latest_mtime = 0;
    for (int i = 0; i < count; ++i) {
        ephoto_photo_t photo = {0};
        if (gallery_service_get_item(i, &photo) != ESP_OK) {
            continue;
        }
        if (latest_index < 0 || photo.mtime >= latest_mtime) {
            latest_index = i;
            latest_mtime = photo.mtime;
        }
    }
    return latest_index;
}

static bool focus_latest_uploaded_photo_locked(void)
{
    int latest_index = -1;
    if (s_pending_uploaded_photo_path[0]) {
        latest_index = gallery_service_find_index_by_path(s_pending_uploaded_photo_path);
    }
    if (latest_index < 0) {
        latest_index = find_latest_photo_index_locked();
    }
    if (latest_index < 0) {
        return false;
    }

    if (s_state.current_photo_index == latest_index) {
        sync_current_photo_locked();
        s_pending_uploaded_photo_path[0] = '\0';
        return true;
    }

    s_state.current_photo_index = latest_index;
    sync_current_photo_locked();
    if (s_state.current_photo_index >= 0) {
        s_pending_uploaded_photo_path[0] = '\0';
        return true;
    }
    return false;
}

static void clear_pending_selected_photo_locked(void)
{
    s_pending_selected_photo_index = -1;
}

static void clear_pending_rebuild_photo_locked(void)
{
    s_pending_rebuild_photo_index = -1;
}

static void clear_pending_uploaded_photo_queue_locked(void)
{
    s_pending_uploaded_photo_count = 0;
    memset(s_pending_uploaded_photo_paths, 0, sizeof(s_pending_uploaded_photo_paths));
}

static bool queue_pending_uploaded_photo_locked(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }

    for (size_t i = 0; i < s_pending_uploaded_photo_count; ++i) {
        if (strcmp(s_pending_uploaded_photo_paths[i], path) == 0) {
            return true;
        }
    }

    if (s_pending_uploaded_photo_count >= (sizeof(s_pending_uploaded_photo_paths) / sizeof(s_pending_uploaded_photo_paths[0]))) {
        ESP_LOGW(TAG, "pending upload queue full, dropping path: %s", path);
        return false;
    }

    strlcpy(s_pending_uploaded_photo_paths[s_pending_uploaded_photo_count],
            path,
            sizeof(s_pending_uploaded_photo_paths[s_pending_uploaded_photo_count]));
    s_pending_uploaded_photo_count += 1;
    return true;
}

static void bump_state_version_locked(void)
{
    ++s_state.state_version;
}

static void notify_ui_task(void)
{
    if (s_ui_task_handle) {
        xTaskNotifyGive(s_ui_task_handle);
    }
}

static void notify_photo_task(void)
{
    if (s_photo_task_handle) {
        xTaskNotifyGive(s_photo_task_handle);
    }
}

static bool command_blocked_while_locked(ephoto_command_type_t type)
{
    switch (type) {
    case EPHOTO_CMD_SHOW_NOTIFICATION:
    case EPHOTO_CMD_REFRESH_WIFI_STATE:
        return false;
    default:
        return true;
    }
}

static void set_cache_build_state_locked(bool active,
                                         bool lock_interaction,
                                         uint32_t total,
                                         uint32_t done,
                                         uint32_t failed,
                                         uint32_t photo_total,
                                         uint32_t photo_index,
                                         const char *photo_name)
{
    s_cache_build_mode = active ? s_cache_build_mode : EPHOTO_CACHE_BUILD_NONE;
    s_state.cache_build_active = active;
    s_state.interaction_locked = active && lock_interaction;
    s_state.cache_build_total = total;
    s_state.cache_build_done = done;
    s_state.cache_build_failed = failed;
    s_state.cache_build_photo_total = photo_total;
    s_state.cache_build_photo_index = photo_index;
    strlcpy(s_state.cache_build_photo_name, photo_name ? photo_name : "", sizeof(s_state.cache_build_photo_name));
}

static void cache_build_progress_callback(void *ctx, uint32_t completed, uint32_t failed)
{
    static int64_t last_notify_ms;
    static uint32_t last_completed;
    static uint32_t last_failed;
    cache_progress_context_t *progress = (cache_progress_context_t *)ctx;
    if (!progress || !s_state_mutex) {
        return;
    }

    int64_t now_ms = clock_service_now_ms();
    bool final_update = completed >= progress->total || failed != last_failed;
    if (!final_update && completed == last_completed && (now_ms - last_notify_ms) < 120) {
        return;
    }
    last_notify_ms = now_ms;
    last_completed = completed;
    last_failed = failed;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    set_cache_build_state_locked(true,
                                 progress->lock_interaction,
                                 progress->total,
                                 progress->base_done + completed,
                                 progress->base_failed + failed,
                                 progress->photo_total,
                                 progress->photo_index,
                                 progress->photo_name);
    bump_state_version_locked();
    xSemaphoreGive(s_state_mutex);

    notify_ui_task();
}

static uint32_t count_missing_cache_jobs_locked(void)
{
    int photo_count = gallery_service_get_count();
    if (photo_count <= 0) {
        return 0;
    }

    /* Each photo currently owns 6 cache artifacts:
     * 4 screen display caches: contain/cover x rotation 0/90
     * 2 web caches: contain preview + cover thumbnail
     */
    static const ephoto_fit_mode_t fit_modes[] = {EPHOTO_FIT_CONTAIN, EPHOTO_FIT_COVER};
    static const uint16_t rotations[] = {0, 90};
    uint32_t total = 0;

    for (int i = 0; i < photo_count; ++i) {
        ephoto_photo_t photo = {0};
        if (gallery_service_get_item(i, &photo) != ESP_OK) {
            continue;
        }
        for (size_t fit_i = 0; fit_i < sizeof(fit_modes) / sizeof(fit_modes[0]); ++fit_i) {
            for (size_t rot_i = 0; rot_i < sizeof(rotations) / sizeof(rotations[0]); ++rot_i) {
                if (!display_service_has_cache(photo.path, fit_modes[fit_i], rotations[rot_i])) {
                    if (display_service_has_known_cache_failure(photo.path, fit_modes[fit_i], rotations[rot_i])) {
                        continue;
                    }
                    ++total;
                }
            }
        }

        for (size_t fit_i = 0; fit_i < sizeof(fit_modes) / sizeof(fit_modes[0]); ++fit_i) {
            if (!display_service_has_web_thumbnail(photo.path, fit_modes[fit_i])) {
                if (display_service_has_known_web_thumbnail_failure(photo.path, fit_modes[fit_i])) {
                    continue;
                }
                ++total;
            }
        }
    }
    return total;
}

static uint32_t count_missing_cache_jobs_for_path_locked(const char *path, bool *has_pending)
{
    static const ephoto_fit_mode_t fit_modes[] = {EPHOTO_FIT_CONTAIN, EPHOTO_FIT_COVER};
    static const uint16_t rotations[] = {0, 90};
    uint32_t total = 0;

    if (has_pending) {
        *has_pending = false;
    }
    if (!path || !path[0]) {
        return 0;
    }

    for (size_t fit_i = 0; fit_i < sizeof(fit_modes) / sizeof(fit_modes[0]); ++fit_i) {
        for (size_t rot_i = 0; rot_i < sizeof(rotations) / sizeof(rotations[0]); ++rot_i) {
            if (!display_service_has_cache(path, fit_modes[fit_i], rotations[rot_i])) {
                if (display_service_has_known_cache_failure(path, fit_modes[fit_i], rotations[rot_i])) {
                    continue;
                }
                total += 1U;
            }
        }
    }

    for (size_t fit_i = 0; fit_i < sizeof(fit_modes) / sizeof(fit_modes[0]); ++fit_i) {
        if (!display_service_has_web_thumbnail(path, fit_modes[fit_i])) {
            if (display_service_has_known_web_thumbnail_failure(path, fit_modes[fit_i])) {
                continue;
            }
            total += 1U;
        }
    }

    if (has_pending) {
        *has_pending = total > 0;
    }
    return total;
}

static uint32_t count_missing_cache_photos_locked(void)
{
    int photo_count = gallery_service_get_count();
    if (photo_count <= 0) {
        return 0;
    }

    static const ephoto_fit_mode_t fit_modes[] = {EPHOTO_FIT_CONTAIN, EPHOTO_FIT_COVER};
    static const uint16_t rotations[] = {0, 90};
    uint32_t total = 0;

    for (int i = 0; i < photo_count; ++i) {
        ephoto_photo_t photo = {0};
        if (gallery_service_get_item(i, &photo) != ESP_OK) {
            continue;
        }

        bool missing_for_photo = false;
        for (size_t fit_i = 0; fit_i < sizeof(fit_modes) / sizeof(fit_modes[0]) && !missing_for_photo; ++fit_i) {
            for (size_t rot_i = 0; rot_i < sizeof(rotations) / sizeof(rotations[0]); ++rot_i) {
                if (!display_service_has_cache(photo.path, fit_modes[fit_i], rotations[rot_i]) &&
                    !display_service_has_known_cache_failure(photo.path, fit_modes[fit_i], rotations[rot_i])) {
                    missing_for_photo = true;
                    break;
                }
            }
        }

        for (size_t fit_i = 0; fit_i < sizeof(fit_modes) / sizeof(fit_modes[0]) && !missing_for_photo; ++fit_i) {
            if (!display_service_has_web_thumbnail(photo.path, fit_modes[fit_i]) &&
                !display_service_has_known_web_thumbnail_failure(photo.path, fit_modes[fit_i])) {
                missing_for_photo = true;
            }
        }

        if (missing_for_photo) {
            ++total;
        }
    }

    return total;
}

static void begin_full_cache_build_locked(bool lock_interaction, const char *photo_name)
{
    uint32_t total = count_missing_cache_jobs_locked();
    uint32_t photo_total = count_missing_cache_photos_locked();
    if (total == 0) {
        s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
        set_cache_build_state_locked(false, false, 0, 0, 0, 0, 0, "");
        return;
    }

    s_cache_build_mode = EPHOTO_CACHE_BUILD_FULL;
    set_cache_build_state_locked(true,
                                 lock_interaction,
                                 total,
                                 0,
                                 0,
                                 photo_total,
                                 photo_total > 0 ? 1U : 0U,
                                 photo_name ? photo_name : s_state.current_photo_name);
}

static void begin_pending_upload_cache_build_locked(const char *photo_name)
{
    uint32_t total = 0;
    uint32_t photo_total = 0;

    for (size_t i = 0; i < s_pending_uploaded_photo_count; ++i) {
        bool has_pending = false;
        total += count_missing_cache_jobs_for_path_locked(s_pending_uploaded_photo_paths[i], &has_pending);
        if (has_pending) {
            photo_total += 1U;
        }
    }

    if (total == 0 || photo_total == 0) {
        clear_pending_uploaded_photo_queue_locked();
        s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
        set_cache_build_state_locked(false, false, 0, 0, 0, 0, 0, "");
        return;
    }

    s_cache_build_mode = EPHOTO_CACHE_BUILD_UPLOAD_BATCH;
    set_cache_build_state_locked(true,
                                 false,
                                 total,
                                 0,
                                 0,
                                 photo_total,
                                 1U,
                                 photo_name ? photo_name : s_state.current_photo_name);
}

static void arm_cache_build_locked(bool lock_interaction, bool upload_batch, const char *photo_name)
{
    s_cache_build_mode = upload_batch ? EPHOTO_CACHE_BUILD_UPLOAD_BATCH : EPHOTO_CACHE_BUILD_FULL;
    set_cache_build_state_locked(true,
                                 lock_interaction,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 photo_name ? photo_name : s_state.current_photo_name);
}

static void set_notification_locked(ephoto_notification_level_t level, const char *text, const char *screen_text)
{
    notification_service_publish(level, text, screen_text);
    notification_service_snapshot(&s_state.notification);
}

static void menu_apply_locked(bool *save_settings,
                              bool *notify_photo,
                              bool close_menu_after_apply,
                              bool keep_editing,
                              bool emit_notification);

static bool is_photo_transition_notification_locked(void)
{
    if (s_state.notification.level != EPHOTO_NOTIFICATION_INFO) {
        return false;
    }

    return strcmp(s_state.notification.screen_text, "上一张") == 0 ||
           strcmp(s_state.notification.screen_text, "下一张") == 0 ||
           strcmp(s_state.notification.screen_text, "显示所选") == 0 ||
           strcmp(s_state.notification.screen_text, "正在旋转") == 0;
}

static bool clear_photo_transition_notification_locked(void)
{
    if (!is_photo_transition_notification_locked()) {
        return false;
    }

    notification_service_clear();
    notification_service_snapshot(&s_state.notification);
    return true;
}

static void notify_wifi_access_locked(const char *prefix)
{
    char note[EPHOTO_MAX_NOTIFICATION_LEN];
    char screen_note[EPHOTO_MAX_NOTIFICATION_LEN];
    const char *ap_ssid = wifi_admin_get_ap_ssid();
    const char *ap_ip = wifi_admin_get_ap_ip();
    const char *sta_ip = wifi_admin_get_sta_ip();
    const char *hostname = wifi_admin_get_hostname();

    if (s_state.network_mode == EPHOTO_NETWORK_AP) {
        snprintf(note,
                 sizeof(note),
                 "%s 热点:%s 地址:%s / http://%s.local",
                 prefix ? prefix : "Wi-Fi 就绪",
                 ap_ssid && ap_ssid[0] ? ap_ssid : "-",
                 ap_ip && ap_ip[0] ? ap_ip : "192.168.4.1",
                 hostname && hostname[0] ? hostname : "ephoto");
        snprintf(screen_note,
                 sizeof(screen_note),
                 "热点 %s",
                 ap_ip && ap_ip[0] ? ap_ip : "192.168.4.1");
    } else if (s_state.network_mode == EPHOTO_NETWORK_STA) {
        snprintf(note,
                 sizeof(note),
                 "%s 已连 %s 地址:%s / http://%s.local",
                 prefix ? prefix : "Wi-Fi 就绪",
                 s_state.current_ssid[0] ? s_state.current_ssid : "Wi-Fi",
                 sta_ip && sta_ip[0] ? sta_ip : "-",
                 hostname && hostname[0] ? hostname : "ephoto");
        snprintf(screen_note,
                 sizeof(screen_note),
                 "已连接 %s",
                 sta_ip && sta_ip[0] ? sta_ip : "Wi-Fi");
    } else {
        const char *mode_label = s_state.network_mode == EPHOTO_NETWORK_DISCONNECTED ? "离线" : "启动中";
        snprintf(note, sizeof(note), "%s 当前网络模式: %s",
                 prefix ? prefix : "Wi-Fi 状态",
                 mode_label);
        snprintf(screen_note, sizeof(screen_note), "网络 %s",
                 mode_label);
    }

    set_notification_locked(EPHOTO_NOTIFICATION_INFO, note, screen_note);
}

static void apply_settings_locked(bool save_settings)
{
    if (save_settings) {
        s_settings_dirty = true;
        s_settings_dirty_at_ms = clock_service_now_ms();
    }
    clock_service_set_timezone(s_state.settings.timezone);
    clock_service_set_auto_sync_enabled(s_state.settings.auto_time_sync);
}

static void sync_menu_snapshot_locked(void)
{
    bool editing = s_state.menu.editing;
    memset(&s_state.menu, 0, sizeof(s_state.menu));
    s_state.menu.visible = s_state.menu_visible;

    if (!s_state.menu_visible) {
        s_state.menu_index = 0;
        return;
    }

    if (s_menu_selected_index >= EPHOTO_MENU_TOTAL_ITEMS) {
        s_menu_selected_index = 0;
    }

    s_state.menu.depth = 0;
    s_state.menu.item_count = EPHOTO_MENU_TOTAL_ITEMS;
    s_state.menu.option_count = EPHOTO_MENU_OPTION_COUNT;
    s_state.menu.selected_index = s_menu_selected_index;
    s_state.menu.editing = editing;
    strlcpy(s_state.menu.title, "设置", sizeof(s_state.menu.title));
    strlcpy(s_state.menu.title_key, "设置", sizeof(s_state.menu.title_key));

    for (size_t i = 0; i < EPHOTO_MENU_TOTAL_ITEMS && i < EPHOTO_MENU_MAX_ITEMS; ++i) {
        char value[EPHOTO_MENU_LABEL_LEN];
        strlcpy(s_state.menu.items[i], menu_item_label(i), sizeof(s_state.menu.items[i]));
        strlcpy(s_state.menu.item_keys[i], menu_item_label(i), sizeof(s_state.menu.item_keys[i]));
        menu_item_value_string_locked(i, value, sizeof(value));
        strlcpy(s_state.menu.item_values[i], value, sizeof(s_state.menu.item_values[i]));
    }

    if (s_state.menu.selected_index < EPHOTO_MENU_TOTAL_ITEMS) {
        size_t selected = s_state.menu.selected_index;
        char value[EPHOTO_MENU_LABEL_LEN];
        strlcpy(s_state.menu.selected_label, menu_item_label(selected), sizeof(s_state.menu.selected_label));
        strlcpy(s_state.menu.selected_key, menu_item_label(selected), sizeof(s_state.menu.selected_key));
        menu_item_value_string_locked(selected, value, sizeof(value));
        strlcpy(s_state.menu.selected_value, value, sizeof(s_state.menu.selected_value));
        strlcpy(s_state.menu.selected_value_key, value, sizeof(s_state.menu.selected_value_key));

        if (editing && menu_index_is_option(selected)) {
            uint8_t count = menu_choice_count(selected);
            if (count > EPHOTO_MENU_MAX_ITEMS) {
                count = EPHOTO_MENU_MAX_ITEMS;
            }
            if (count > 0 && s_menu_edit_choice_index >= count) {
                s_menu_edit_choice_index = 0;
            }
            s_state.menu.edit_choice_count = count;
            s_state.menu.edit_choice_index = s_menu_edit_choice_index;
            for (uint8_t i = 0; i < count; ++i) {
                char choice[EPHOTO_MENU_LABEL_LEN];
                menu_choice_string(selected, i, choice, sizeof(choice));
                strlcpy(s_state.menu.edit_choices[i], choice, sizeof(s_state.menu.edit_choices[i]));
            }
        }
    }

    s_state.menu_index = s_state.menu.selected_index;
}

static void refresh_menu_selection_locked(void)
{
    if (!s_state.menu_visible) {
        return;
    }

    if (s_menu_selected_index >= EPHOTO_MENU_TOTAL_ITEMS) {
        s_menu_selected_index = 0;
    }

    s_state.menu.selected_index = s_menu_selected_index;
    s_state.menu_index = s_menu_selected_index;

    if (s_menu_selected_index < EPHOTO_MENU_TOTAL_ITEMS) {
        char value[EPHOTO_MENU_LABEL_LEN];
        strlcpy(s_state.menu.selected_label,
                menu_item_label(s_menu_selected_index),
                sizeof(s_state.menu.selected_label));
        strlcpy(s_state.menu.selected_key,
                menu_item_label(s_menu_selected_index),
                sizeof(s_state.menu.selected_key));
        menu_item_value_string_locked(s_menu_selected_index, value, sizeof(value));
        strlcpy(s_state.menu.selected_value, value, sizeof(s_state.menu.selected_value));
        strlcpy(s_state.menu.selected_value_key, value, sizeof(s_state.menu.selected_value_key));
    }
}

static void refresh_menu_edit_state_locked(bool rebuild_choices)
{
    if (!s_state.menu_visible) {
        return;
    }

    refresh_menu_selection_locked();
    s_state.menu.editing = true;

    if (s_menu_selected_index < EPHOTO_MENU_TOTAL_ITEMS) {
        char value[EPHOTO_MENU_LABEL_LEN];
        menu_item_value_string_locked(s_menu_selected_index, value, sizeof(value));
        strlcpy(s_state.menu.item_values[s_menu_selected_index],
                value,
                sizeof(s_state.menu.item_values[s_menu_selected_index]));
        strlcpy(s_state.menu.selected_value, value, sizeof(s_state.menu.selected_value));
        strlcpy(s_state.menu.selected_value_key, value, sizeof(s_state.menu.selected_value_key));
    }

    if (!menu_index_is_option(s_menu_selected_index)) {
        s_state.menu.edit_choice_count = 0;
        s_state.menu.edit_choice_index = 0;
        return;
    }

    uint8_t count = menu_choice_count(s_menu_selected_index);
    if (count > EPHOTO_MENU_MAX_ITEMS) {
        count = EPHOTO_MENU_MAX_ITEMS;
    }
    if (count > 0 && s_menu_edit_choice_index >= count) {
        s_menu_edit_choice_index = 0;
    }
    s_state.menu.edit_choice_count = count;
    s_state.menu.edit_choice_index = s_menu_edit_choice_index;

    if (rebuild_choices) {
        for (uint8_t i = 0; i < count; ++i) {
            char choice[EPHOTO_MENU_LABEL_LEN];
            menu_choice_string(s_menu_selected_index, i, choice, sizeof(choice));
            strlcpy(s_state.menu.edit_choices[i], choice, sizeof(s_state.menu.edit_choices[i]));
        }
    }
}

static void menu_open_locked(void)
{
    s_menu_selected_index = 0;
    s_menu_edit_choice_index = 0;
    s_menu_original_settings = s_state.settings;
    s_menu_draft_settings = s_state.settings;
    s_state.menu_visible = true;
    s_state.menu.editing = false;
    s_state.menu_last_input_ms = clock_service_now_ms();
    sync_menu_snapshot_locked();
    set_notification_locked(EPHOTO_NOTIFICATION_INFO, "菜单已打开", "菜单已打开");
}

static void menu_close_locked(const char *message)
{
    s_state.menu_visible = false;
    s_menu_edit_choice_index = 0;
    memset(&s_state.menu, 0, sizeof(s_state.menu));
    s_state.menu_index = 0;
    if (message && message[0]) {
        set_notification_locked(EPHOTO_NOTIFICATION_INFO, message, "菜单已关闭");
    }
}

static void menu_step_locked(int delta)
{
    if (s_state.menu.editing && menu_index_is_option(s_menu_selected_index)) {
        uint8_t count = menu_choice_count(s_menu_selected_index);
        if (count > 0) {
            int next = (int)s_menu_edit_choice_index + delta;
            while (next < 0) {
                next += count;
            }
            s_menu_edit_choice_index = (uint8_t)(next % count);
            menu_apply_choice_locked(s_menu_selected_index, s_menu_edit_choice_index);
            bool save_settings = false;
            bool notify_photo = false;
            menu_apply_locked(&save_settings, &notify_photo, false, true, true);
        }
    } else {
        int next = (int)s_menu_selected_index + delta;
        while (next < 0) {
            next += (int)EPHOTO_MENU_TOTAL_ITEMS;
        }
        s_menu_selected_index = (uint8_t)(next % (int)EPHOTO_MENU_TOTAL_ITEMS);
    }

    s_state.menu_last_input_ms = clock_service_now_ms();
    if (!s_state.menu.editing) {
        refresh_menu_selection_locked();
    }
}

static void menu_apply_locked(bool *save_settings,
                              bool *notify_photo,
                              bool close_menu_after_apply,
                              bool keep_editing,
                              bool emit_notification)
{
    ephoto_settings_t prev_settings = s_state.settings;
    bool settings_changed = !settings_equal(&s_state.settings, &s_menu_draft_settings);
    bool orientation_changed = settings_changed &&
                               (prev_settings.orientation_filter != s_menu_draft_settings.orientation_filter);
    bool visual_changed = false;

    if (settings_changed) {
        s_state.settings = s_menu_draft_settings;
        refresh_effective_rotation_locked();
        *save_settings = true;
        visual_changed = prev_settings.fit_mode != s_state.settings.fit_mode ||
                         orientation_changed ||
                         prev_settings.rotation_deg != s_state.settings.rotation_deg;
        if (orientation_changed) {
            sync_storage_locked();
            if (s_state.storage.photo_count > 0) {
                if (s_state.current_photo_index < 0) {
                    s_state.current_photo_index = choose_photo_index_locked(-1, 1);
                } else {
                    ephoto_photo_t current = {0};
                    if (gallery_service_get_item(s_state.current_photo_index, &current) != ESP_OK ||
                        !photo_matches_orientation_filter(&current, s_state.settings.orientation_filter)) {
                        s_state.current_photo_index = choose_photo_index_locked(s_state.current_photo_index, 1);
                    }
                }
            } else {
                s_state.current_photo_index = -1;
            }
            sync_current_photo_locked();
        }
        if (visual_changed) {
            *notify_photo = true;
        }
    }

    s_menu_draft_settings.rotation_deg = s_state.settings.rotation_deg;
    s_menu_original_settings = s_state.settings;
    s_menu_draft_settings = s_state.settings;
    s_state.menu.editing = keep_editing;
    if (close_menu_after_apply) {
        menu_close_locked(NULL);
    } else if (keep_editing) {
        refresh_menu_edit_state_locked(false);
    } else {
        sync_menu_snapshot_locked();
    }
    if (!emit_notification) {
        return;
    }

    if (settings_changed) {
        set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "已应用菜单设置", "设置已应用");
    }
}

static void menu_back_locked(void)
{
    if (s_state.menu.editing) {
        s_state.menu.editing = false;
        s_menu_edit_choice_index = 0;
        s_state.menu_last_input_ms = clock_service_now_ms();
        sync_menu_snapshot_locked();
        set_notification_locked(EPHOTO_NOTIFICATION_INFO, "选项已关闭", "选项已关闭");
        return;
    }

    s_state.menu.editing = false;
    menu_close_locked("设置已关闭");
}

static void menu_confirm_locked(bool *save_settings, bool *notify_photo)
{
    size_t selected = s_menu_selected_index;
    (void)save_settings;
    (void)notify_photo;

    if (selected == EPHOTO_MENU_ITEM_ALBUM) {
        album_open_locked();
        return;
    }

    if (menu_index_is_option(selected)) {
        if (s_state.menu.editing) {
            s_state.menu_last_input_ms = clock_service_now_ms();
            s_state.menu.editing = false;
            sync_menu_snapshot_locked();
        } else {
            s_menu_edit_choice_index = menu_current_choice_index_locked(selected);
            s_state.menu.editing = true;
            s_state.menu_last_input_ms = clock_service_now_ms();
            sync_menu_snapshot_locked();
        }
        return;
    }
}

static void handle_hw_confirm_locked(bool *save_settings, bool *notify_photo, bool *notify_ui)
{
    if (s_album_visible) {
        if (!s_album_action_popup_visible) {
            s_album_action_index = 0;
            album_action_popup_set_locked(true);
        } else {
            if (s_album_action_index == 0) {
                if (s_album_selected_index >= 0) {
                    if (s_album_selected_index == s_state.current_photo_index) {
                        album_close_locked(NULL);
                        set_notification_locked(EPHOTO_NOTIFICATION_INFO, "当前照片已在播放", "保持当前");
                    } else {
                        s_pending_selected_photo_index = s_album_selected_index;
                        album_close_locked(NULL);
                        set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在准备所选照片", "显示所选");
                        *notify_photo = true;
                    }
                }
            } else if (s_album_action_index >= 1 && s_album_action_index <= 4) {
                uint16_t rotation_deg = 0;
                if (album_action_rotation_target(s_album_action_index, &rotation_deg)) {
                    if (album_action_needs_rebuild_confirmation(s_album_action_index) &&
                        (!s_album_action_confirm_pending || s_album_action_confirm_index != s_album_action_index)) {
                        s_album_action_confirm_pending = true;
                        s_album_action_confirm_index = s_album_action_index;
                        album_sync_snapshot_locked();
                        *notify_ui = true;
                        return;
                    }
                    s_album_action_confirm_pending = false;
                    s_album_action_confirm_index = 0;
                    album_close_locked(NULL);
                    esp_err_t adjust_err = apply_photo_orientation_adjustment_locked(s_album_selected_index,
                                                                                     rotation_deg);
                    if (adjust_err == ESP_OK) {
                        char detail[EPHOTO_MAX_NOTIFICATION_LEN];
                        snprintf(detail,
                                 sizeof(detail),
                                 "已设置为%s，正在重建这张照片缓存",
                                 photo_rotation_action_title(rotation_deg));
                        set_notification_locked(EPHOTO_NOTIFICATION_INFO,
                                                detail,
                                                photo_rotation_action_title(rotation_deg));
                        *notify_photo = true;
                    } else if (adjust_err == ESP_ERR_NOT_SUPPORTED) {
                        set_notification_locked(EPHOTO_NOTIFICATION_ERROR,
                                                "当前只有 JPEG 照片支持方向修正",
                                                "暂不支持");
                    } else {
                        set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "调整照片方向失败", "调整失败");
                    }
                }
            } else {
                bool current_changed = false;
                esp_err_t delete_err = delete_photo_index_locked(s_album_selected_index, &current_changed);
                if (delete_err == ESP_OK) {
                    set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "照片已删除", "删除完成");
                    *notify_photo = current_changed;
                } else {
                    set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "删除照片失败", "删除失败");
                }
            }
        }
        *notify_ui = true;
        return;
    }

    if (s_state.menu_visible) {
        menu_confirm_locked(save_settings, notify_photo);
        *notify_ui = true;
        return;
    }

    menu_open_locked();
    *notify_ui = true;
}

static void handle_hw_prev_locked(bool *notify_ui, bool *notify_photo)
{
    clear_pending_selected_photo_locked();
    clear_pending_rebuild_photo_locked();

    if (s_album_visible) {
        if (s_album_action_popup_visible) {
            album_cycle_action_locked(-1);
        } else {
            album_move_selection_locked(-1);
        }
        *notify_ui = true;
        return;
    }

    if (s_state.menu_visible) {
        menu_step_locked(-1);
        *notify_ui = true;
        return;
    }

    if (s_state.storage.photo_count <= 0) {
        return;
    }

    s_state.current_photo_index = choose_photo_index_locked(s_state.current_photo_index, -1);
    sync_current_photo_locked();
    set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在切换到上一张", "上一张");
    *notify_ui = true;
    *notify_photo = true;
}

static void handle_hw_next_locked(bool *notify_ui, bool *notify_photo)
{
    clear_pending_selected_photo_locked();
    clear_pending_rebuild_photo_locked();

    if (s_album_visible) {
        if (s_album_action_popup_visible) {
            album_cycle_action_locked(1);
        } else {
            album_move_selection_locked(1);
        }
        *notify_ui = true;
        return;
    }

    if (s_state.menu_visible) {
        menu_step_locked(1);
        *notify_ui = true;
        return;
    }

    if (s_state.storage.photo_count <= 0) {
        return;
    }

    s_state.current_photo_index = choose_photo_index_locked(s_state.current_photo_index, 1);
    sync_current_photo_locked();
    set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在切换到下一张", "下一张");
    *notify_ui = true;
    *notify_photo = true;
}

static void handle_hw_rotate_locked(bool *save_settings, bool *notify_ui, bool *notify_photo)
{
    clear_pending_selected_photo_locked();
    clear_pending_rebuild_photo_locked();

    if (s_album_visible) {
        if (s_album_action_popup_visible) {
            album_action_popup_set_locked(false);
        } else {
            album_close_locked("已退出相册");
        }
        *notify_ui = true;
        return;
    }

    if (s_state.menu_visible) {
        menu_back_locked();
        *notify_ui = true;
        return;
    }

    if (manual_rotation_blocked_by_auto_locked()) {
        set_notification_locked(EPHOTO_NOTIFICATION_INFO,
                                "当前已开启自动旋转。请先进入设置关闭自动旋转，再使用旋转按键手动切换横竖方向",
                                "请先关闭自动旋转");
        *notify_ui = true;
        return;
    }

    s_state.settings.manual_rotation_deg = s_state.settings.manual_rotation_deg == 0 ? 90 : 0;
    bool changed = refresh_effective_rotation_locked();
    *save_settings = true;
    set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在切换方向并装载图片", "正在旋转");
    *notify_ui = true;
    *notify_photo = changed;
}

static void handle_hw_zoom_locked(bool *save_settings, bool *notify_ui, bool *notify_photo)
{
    clear_pending_selected_photo_locked();
    clear_pending_rebuild_photo_locked();

    if (s_album_visible) {
        album_close_locked("已退出相册");
        *notify_ui = true;
        return;
    }

    if (s_state.menu_visible) {
        menu_close_locked("设置已关闭");
        *notify_ui = true;
        return;
    }

    s_state.settings.fit_mode = s_state.settings.fit_mode == EPHOTO_FIT_COVER
                                    ? EPHOTO_FIT_CONTAIN
                                    : EPHOTO_FIT_COVER;
    *save_settings = true;
    *notify_ui = true;
    *notify_photo = true;
}

static void process_command(const ephoto_command_t *command)
{
    bool save_settings = false;
    bool notify_ui = false;
    bool notify_photo = false;

    if (command->type == EPHOTO_CMD_CONNECT_WIFI) {
        esp_err_t wifi_err = wifi_admin_connect(command->ssid, command->password);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        sync_network_locked();
        if (wifi_err == ESP_OK) {
            set_notification_locked(EPHOTO_NOTIFICATION_INFO, "Wi-Fi 正在连接，请等待网络恢复", "正在切换网络");
        } else {
            set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "Wi-Fi 切换失败", "网络切换失败");
        }
        bump_state_version_locked();
        xSemaphoreGive(s_state_mutex);
        notify_ui_task();
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_state.interaction_locked && command_blocked_while_locked(command->type)) {
        set_notification_locked(EPHOTO_NOTIFICATION_INFO, "启动缓存处理中，请稍候", "缓存处理中");
        bump_state_version_locked();
        xSemaphoreGive(s_state_mutex);
        notify_ui_task();
        return;
    }
    switch (command->type) {
    case EPHOTO_CMD_HW_CONFIRM:
        handle_hw_confirm_locked(&save_settings, &notify_photo, &notify_ui);
        break;
    case EPHOTO_CMD_HW_PREV:
        handle_hw_prev_locked(&notify_ui, &notify_photo);
        break;
    case EPHOTO_CMD_HW_NEXT:
        handle_hw_next_locked(&notify_ui, &notify_photo);
        break;
    case EPHOTO_CMD_HW_ROTATE:
        handle_hw_rotate_locked(&save_settings, &notify_ui, &notify_photo);
        break;
    case EPHOTO_CMD_HW_ZOOM:
        handle_hw_zoom_locked(&save_settings, &notify_ui, &notify_photo);
        break;
    case EPHOTO_CMD_CONFIRM:
        ESP_LOGI(TAG,
                 "cmd_confirm: album_visible=%d menu_visible=%d selected=%d action=%u pending=%d",
                 s_album_visible,
                 s_state.menu_visible,
                 s_album_selected_index,
                 (unsigned)s_album_action_index,
                 s_pending_selected_photo_index);
        if (s_album_visible) {
            if (!s_album_action_popup_visible) {
                s_album_action_index = 0;
                album_action_popup_set_locked(true);
            } else {
                if (s_album_action_index == 0) {
                    if (s_album_selected_index >= 0) {
                        if (s_album_selected_index == s_state.current_photo_index) {
                            album_close_locked(NULL);
                            set_notification_locked(EPHOTO_NOTIFICATION_INFO, "当前照片已在播放", "保持当前");
                        } else {
                            s_pending_selected_photo_index = s_album_selected_index;
                            album_close_locked(NULL);
                            set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在准备所选照片", "显示所选");
                            notify_photo = true;
                        }
                    }
                } else if (s_album_action_index >= 1 && s_album_action_index <= 4) {
                    uint16_t rotation_deg = 0;
                    if (album_action_rotation_target(s_album_action_index, &rotation_deg)) {
                        if (album_action_needs_rebuild_confirmation(s_album_action_index) &&
                            (!s_album_action_confirm_pending || s_album_action_confirm_index != s_album_action_index)) {
                            s_album_action_confirm_pending = true;
                            s_album_action_confirm_index = s_album_action_index;
                            album_sync_snapshot_locked();
                            notify_ui = true;
                            break;
                        }
                        s_album_action_confirm_pending = false;
                        s_album_action_confirm_index = 0;
                        album_close_locked(NULL);
                        esp_err_t adjust_err = apply_photo_orientation_adjustment_locked(s_album_selected_index,
                                                                                         rotation_deg);
                        if (adjust_err == ESP_OK) {
                            char detail[EPHOTO_MAX_NOTIFICATION_LEN];
                            snprintf(detail,
                                     sizeof(detail),
                                     "已设置为%s，正在重建这张照片缓存",
                                     photo_rotation_action_title(rotation_deg));
                            set_notification_locked(EPHOTO_NOTIFICATION_INFO,
                                                    detail,
                                                    photo_rotation_action_title(rotation_deg));
                            notify_photo = true;
                        } else if (adjust_err == ESP_ERR_NOT_SUPPORTED) {
                            set_notification_locked(EPHOTO_NOTIFICATION_ERROR,
                                                    "当前只有 JPEG 照片支持方向修正",
                                                    "暂不支持");
                        } else {
                            set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "调整照片方向失败", "调整失败");
                        }
                    }
                } else {
                    bool current_changed = false;
                    esp_err_t delete_err = delete_photo_index_locked(s_album_selected_index, &current_changed);
                    if (delete_err == ESP_OK) {
                        set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "照片已删除", "删除完成");
                        notify_photo = current_changed;
                    } else {
                        set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "删除照片失败", "删除失败");
                    }
                }
            }
            notify_ui = true;
        } else if (s_state.menu_visible) {
            menu_confirm_locked(&save_settings, &notify_photo);
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_MENU_LONGPRESS:
        if (s_album_visible) {
            if (s_album_action_popup_visible) {
                album_action_popup_set_locked(false);
            } else {
                album_close_locked("已退出相册");
            }
        } else if (s_state.menu_visible) {
            menu_back_locked();
        } else {
            menu_open_locked();
        }
        notify_ui = true;
        break;
    case EPHOTO_CMD_MENU_CLOSE:
        if (s_album_visible) {
            album_close_locked(command->text[0] ? command->text : "已退出相册");
            notify_ui = true;
        } else if (s_state.menu_visible) {
            menu_close_locked(command->text[0] ? command->text : "已退出菜单");
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_PREV:
        clear_pending_selected_photo_locked();
        clear_pending_rebuild_photo_locked();
        if (s_album_visible) {
            if (s_album_action_popup_visible) {
                album_cycle_action_locked(-1);
            } else {
                album_move_selection_locked(-1);
            }
            notify_ui = true;
        } else if (s_state.menu_visible) {
            menu_step_locked(-1);
            notify_ui = true;
        } else {
            s_state.current_photo_index = choose_photo_index_locked(s_state.current_photo_index, -1);
            sync_current_photo_locked();
            set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在切换到上一张", "上一张");
            notify_ui = true;
            notify_photo = true;
        }
        break;
    case EPHOTO_CMD_NEXT:
        clear_pending_selected_photo_locked();
        clear_pending_rebuild_photo_locked();
        if (s_album_visible) {
            if (s_album_action_popup_visible) {
                album_cycle_action_locked(1);
            } else {
                album_move_selection_locked(1);
            }
            notify_ui = true;
        } else if (s_state.menu_visible) {
            menu_step_locked(1);
            notify_ui = true;
        } else {
            if (s_state.storage.photo_count <= 0) {
                break;
            }
            s_state.current_photo_index = choose_photo_index_locked(s_state.current_photo_index, 1);
            sync_current_photo_locked();
            set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在切换到下一张", "下一张");
            notify_ui = true;
            notify_photo = true;
        }
        break;
    case EPHOTO_CMD_SHOW_PHOTO_INDEX:
        if (!s_state.menu_visible && !s_album_visible && command->value_i32 >= 0) {
            ephoto_photo_t requested = {0};
            esp_err_t get_err = gallery_service_get_item(command->value_i32, &requested);
            if (get_err == ESP_OK &&
                photo_matches_orientation_filter(&requested, s_state.settings.orientation_filter)) {
                s_pending_selected_photo_index = command->value_i32;
                clear_pending_rebuild_photo_locked();
                set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在准备所选照片", "显示所选");
                notify_ui = true;
                notify_photo = true;
            } else if (get_err == ESP_OK) {
                char detail[EPHOTO_MAX_NOTIFICATION_LEN];
                orientation_filter_block_message(&requested,
                                                 s_state.settings.orientation_filter,
                                                 detail,
                                                 sizeof(detail));
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, detail, "方向不符");
                notify_ui = true;
            } else {
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "未找到要显示的照片", "照片错误");
                notify_ui = true;
            }
        }
        break;
    case EPHOTO_CMD_REBUILD_PHOTO_INDEX:
        if (!s_state.menu_visible && !s_album_visible && command->value_i32 >= 0) {
            ephoto_photo_t requested = {0};
            if (gallery_service_get_item(command->value_i32, &requested) == ESP_OK) {
                clear_pending_selected_photo_locked();
                s_pending_rebuild_photo_index = command->value_i32;
                display_service_invalidate_photo_cache(requested.path);
                s_cache_build_mode = EPHOTO_CACHE_BUILD_REBUILD_SINGLE;
                set_cache_build_state_locked(true, false, 6, 0, 0, 1, 1, requested.name);
                set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在重建该照片缓存", "重建缓存");
                notify_ui = true;
                notify_photo = true;
            } else {
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "未找到要重建的照片", "照片错误");
                notify_ui = true;
            }
        }
        break;
    case EPHOTO_CMD_DELETE_PHOTO_INDEX:
        if (!s_state.menu_visible && command->value_i32 >= 0) {
            bool current_changed = false;
            esp_err_t delete_err = delete_photo_index_locked(command->value_i32, &current_changed);
            if (delete_err == ESP_OK) {
                clear_photo_transition_notification_locked();
                set_delete_notification_locked(current_changed);
                notify_photo = current_changed;
            } else {
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "删除照片失败", "删除失败");
            }
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_DELETE_PHOTO_PATH:
        if (!s_state.menu_visible && command->path[0]) {
            bool current_changed = false;
            esp_err_t delete_err = delete_photo_path_locked(command->path, &current_changed);
            if (delete_err == ESP_OK) {
                clear_photo_transition_notification_locked();
                set_delete_notification_locked(current_changed);
                notify_photo = current_changed;
            } else {
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "删除照片失败", "删除失败");
            }
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_SET_PHOTO_ROTATION_90_INDEX:
    case EPHOTO_CMD_SET_PHOTO_ROTATION_180_INDEX:
    case EPHOTO_CMD_SET_PHOTO_ROTATION_270_INDEX:
    case EPHOTO_CMD_RESET_PHOTO_ORIENTATION_INDEX:
        if (!s_state.menu_visible && command->value_i32 >= 0) {
            uint16_t rotation_deg = 0;
            switch (command->type) {
            case EPHOTO_CMD_SET_PHOTO_ROTATION_90_INDEX:
                rotation_deg = 90;
                break;
            case EPHOTO_CMD_SET_PHOTO_ROTATION_180_INDEX:
                rotation_deg = 180;
                break;
            case EPHOTO_CMD_SET_PHOTO_ROTATION_270_INDEX:
                rotation_deg = 270;
                break;
            case EPHOTO_CMD_RESET_PHOTO_ORIENTATION_INDEX:
            default:
                rotation_deg = 0;
                break;
            }
            esp_err_t adjust_err = apply_photo_orientation_adjustment_locked(command->value_i32,
                                                                             rotation_deg);
            if (adjust_err == ESP_OK) {
                char detail[EPHOTO_MAX_NOTIFICATION_LEN];
                snprintf(detail,
                         sizeof(detail),
                         "已设置为%s，正在重建这张照片缓存",
                         photo_rotation_action_title(rotation_deg));
                clear_photo_transition_notification_locked();
                set_notification_locked(EPHOTO_NOTIFICATION_INFO,
                                        detail,
                                        photo_rotation_action_title(rotation_deg));
                notify_photo = true;
            } else if (adjust_err == ESP_ERR_NOT_SUPPORTED) {
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR,
                                        "当前只有 JPEG 照片支持方向修正",
                                        "暂不支持");
            } else {
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "调整照片方向失败", "调整失败");
            }
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_ROTATE:
        clear_pending_selected_photo_locked();
        clear_pending_rebuild_photo_locked();
        if (s_album_visible) {
            if (s_album_action_popup_visible) {
                album_cycle_action_locked(-1);
            } else {
                album_move_selection_locked(-(int)album_columns_locked());
            }
            notify_ui = true;
        } else if (manual_rotation_blocked_by_auto_locked()) {
            set_notification_locked(EPHOTO_NOTIFICATION_INFO,
                                    "当前已开启自动旋转。请先进入设置关闭自动旋转，再使用旋转按键手动切换横竖方向",
                                    "请先关闭自动旋转");
            notify_ui = true;
        } else {
            s_state.settings.manual_rotation_deg = s_state.settings.manual_rotation_deg == 0 ? 90 : 0;
            bool changed = refresh_effective_rotation_locked();
            save_settings = true;
            set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在切换方向并装载图片", "正在旋转");
            notify_ui = true;
            notify_photo = changed;
        }
        break;
    case EPHOTO_CMD_FIT_TOGGLE:
        clear_pending_selected_photo_locked();
        clear_pending_rebuild_photo_locked();
        if (s_state.menu_visible) {
            menu_back_locked();
            notify_ui = true;
        } else if (!s_album_visible) {
            s_state.settings.fit_mode = s_state.settings.fit_mode == EPHOTO_FIT_COVER
                                            ? EPHOTO_FIT_CONTAIN
                                            : EPHOTO_FIT_COVER;
            save_settings = true;
            notify_ui = true;
            notify_photo = true;
        }
        break;
    case EPHOTO_CMD_SCREEN_TOGGLE:
        if (s_album_visible) {
            if (s_album_action_popup_visible) {
                album_cycle_action_locked(1);
            } else {
                album_move_selection_locked((int)album_columns_locked());
            }
            notify_ui = true;
        } else {
            s_state.settings.screen_on = !s_state.settings.screen_on;
            save_settings = true;
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_BRIGHTNESS_STEP:
        if (s_album_visible) {
            album_cycle_action_locked(-1);
            notify_ui = true;
        } else {
            set_manual_brightness_locked(next_brightness_level(s_state.settings.manual_brightness));
            save_settings = true;
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_CLOCK_TOGGLE:
        if (s_album_visible) {
            album_cycle_action_locked(1);
            notify_ui = true;
        } else {
            s_state.settings.clock_visible = !s_state.settings.clock_visible;
            save_settings = true;
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_START_WIFI_AP:
        wifi_admin_set_mode(EPHOTO_NETWORK_AP);
        sync_network_locked();
        notify_wifi_access_locked("已进入热点模式");
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_PLAYBACK_MODE:
        s_state.settings.playback_mode = (ephoto_playback_mode_t)command->value_i32;
        save_settings = true;
        notify_ui = true;
        notify_photo = true;
        break;
    case EPHOTO_CMD_SET_INTERVAL:
        s_state.settings.slideshow_interval = (ephoto_slideshow_interval_t)command->value_i32;
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_ROTATION:
        s_state.settings.manual_rotation_deg = command->value_i32 == 90 ? 90 : 0;
        notify_photo = refresh_effective_rotation_locked();
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_AUTO_ROTATION:
        s_state.settings.auto_rotation_enabled = command->value_i32 != 0;
        notify_photo = refresh_effective_rotation_locked();
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_BRIGHTNESS:
        set_manual_brightness_locked(brightness_from_i32(command->value_i32));
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_BRIGHTNESS_MODE:
        s_state.settings.brightness_mode = command->value_i32 == (int32_t)EPHOTO_BRIGHTNESS_MODE_AUTO
                                               ? EPHOTO_BRIGHTNESS_MODE_AUTO
                                               : EPHOTO_BRIGHTNESS_MODE_MANUAL;
        if (s_state.settings.brightness_mode == EPHOTO_BRIGHTNESS_MODE_MANUAL) {
            s_state.settings.brightness = normalize_brightness(s_state.settings.manual_brightness);
        } else {
            ambient_light_status_t light_status = {0};
            if (ambient_light_service_poll(&light_status) == ESP_OK) {
                sync_ambient_light_locked(&light_status);
                s_state.settings.brightness = auto_brightness_from_percent(light_status.percent);
            }
        }
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_SCREEN_SCHEDULE_ENABLED:
        s_state.settings.screen_schedule_enabled = command->value_i32 != 0;
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_SCREEN_ON_MINUTE:
        s_state.settings.screen_on_minute = normalize_minute_of_day(command->value_i32);
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_SCREEN_OFF_MINUTE:
        s_state.settings.screen_off_minute = normalize_minute_of_day(command->value_i32);
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_FIT_MODE:
        clear_pending_selected_photo_locked();
        s_state.settings.fit_mode = (ephoto_fit_mode_t)command->value_i32;
        save_settings = true;
        notify_ui = true;
        notify_photo = true;
        break;
    case EPHOTO_CMD_SET_ORIENTATION_FILTER:
    {
        clear_pending_selected_photo_locked();
        ephoto_orientation_filter_t next_filter = (ephoto_orientation_filter_t)command->value_i32;
        if (s_state.settings.orientation_filter != next_filter) {
            s_state.settings.orientation_filter = next_filter;
            save_settings = true;
            sync_storage_locked();

            bool keep_current = false;
            if (s_state.current_photo_index >= 0) {
                ephoto_photo_t current_photo = {0};
                if (gallery_service_get_item(s_state.current_photo_index, &current_photo) == ESP_OK) {
                    keep_current = photo_matches_orientation_filter(&current_photo, next_filter);
                }
            }

            if (!keep_current) {
                if (s_state.storage.photo_count > 0) {
                    s_state.current_photo_index = choose_photo_index_locked(s_state.current_photo_index, 1);
                } else {
                    s_state.current_photo_index = -1;
                }
                sync_current_photo_locked();
            } else {
                sync_current_photo_locked();
            }

            notify_ui = true;
            notify_photo = true;
        }
        break;
    }
    case EPHOTO_CMD_SET_SCREEN_ON:
        s_state.settings.screen_on = command->value_i32 != 0;
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_CLOCK_VISIBLE:
        s_state.settings.clock_visible = command->value_i32 != 0;
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_CLOCK_FORMAT:
        s_state.settings.clock_format = command->value_i32 == (int32_t)EPHOTO_CLOCK_FORMAT_12H
                                            ? EPHOTO_CLOCK_FORMAT_12H
                                            : EPHOTO_CLOCK_FORMAT_24H;
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_CLOCK_POSITION:
        s_state.settings.clock_position = normalize_clock_position((ephoto_clock_position_t)command->value_i32);
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_CLOCK_COLOR:
        s_state.settings.clock_color = normalize_clock_color((ephoto_clock_color_t)command->value_i32);
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_OTA_INTERVAL:
        if (command->value_i32 >= (int32_t)EPHOTO_OTA_INTERVAL_OFF &&
            command->value_i32 <= (int32_t)EPHOTO_OTA_INTERVAL_1W) {
            s_state.settings.ota_interval = (ephoto_ota_interval_t)command->value_i32;
            save_settings = true;
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_SET_OTA_CHANNEL:
        if (command->text[0]) {
            strlcpy(s_state.settings.ota_channel, command->text, sizeof(s_state.settings.ota_channel));
        } else {
            strlcpy(s_state.settings.ota_channel, "stable", sizeof(s_state.settings.ota_channel));
        }
        ESP_LOGI(TAG, "set ota channel command applied: %s", s_state.settings.ota_channel);
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_OTA_CHECK_NOW:
    {
        ephoto_settings_t ota_settings = s_state.settings;
        if (strcmp(command->text, "beta") == 0) {
            strlcpy(ota_settings.ota_channel, "beta", sizeof(ota_settings.ota_channel));
        } else {
            strlcpy(ota_settings.ota_channel, "stable", sizeof(ota_settings.ota_channel));
        }
        esp_err_t ota_err = ota_service_request_check(true, &ota_settings, wifi_admin_is_sta_connected());
        if (ota_err == ESP_OK) {
            set_notification_locked(EPHOTO_NOTIFICATION_INFO,
                                    strcmp(ota_settings.ota_channel, "beta") == 0 ? "正在检查开发版更新" : "正在检查稳定版更新",
                                    "检查更新");
        } else {
            const char *message = wifi_admin_is_sta_connected()
                                      ? "无法开始检查更新，请稍后重试"
                                      : "设备未连接家庭网络，无法访问 OTA 服务";
            set_notification_locked(EPHOTO_NOTIFICATION_ERROR, message, "更新失败");
        }
        notify_ui = true;
        break;
    }
    case EPHOTO_CMD_OTA_UPDATE_NOW:
    {
        esp_err_t ota_err = ota_service_request_update(true, &s_state.settings, wifi_admin_is_sta_connected());
        if (ota_err == ESP_OK) {
            set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在下载并安装更新", "安装更新");
        } else {
            set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "无法开始安装更新", "更新失败");
        }
        notify_ui = true;
        break;
    }
    case EPHOTO_CMD_SET_TIMEZONE:
        if (command->text[0]) {
            strlcpy(s_state.settings.timezone, command->text, sizeof(s_state.settings.timezone));
            save_settings = true;
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_SET_AUTO_TIME_SYNC:
        s_state.settings.auto_time_sync = command->value_i32 != 0;
        save_settings = true;
        notify_ui = true;
        break;
    case EPHOTO_CMD_SYNC_TIME_NOW:
        if (clock_service_sync_now() == ESP_OK) {
            set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "已开始联网校时", "联网校时");
        } else {
            set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "联网校时启动失败", "校时失败");
        }
        notify_ui = true;
        break;
    case EPHOTO_CMD_SET_MANUAL_TIME:
        if (clock_service_set_manual_time_string(command->text) == ESP_OK) {
            set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "时间已手动校准", "时间已设置");
        } else {
            set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "手动校时失败", "校时失败");
        }
        notify_ui = true;
        break;
    case EPHOTO_CMD_REFRESH_WIFI_STATE:
        sync_network_locked();
        notify_ui = true;
        break;
    case EPHOTO_CMD_REPROCESS_CURRENT:
        if (s_state.current_photo_path[0]) {
            clear_pending_rebuild_photo_locked();
            display_service_invalidate_photo_cache(s_state.current_photo_path);
            begin_full_cache_build_locked(true, s_state.current_photo_name);
            set_notification_locked(EPHOTO_NOTIFICATION_INFO, "正在重新生成图片缓存", "重建缓存");
            notify_ui = true;
            notify_photo = true;
        } else {
            set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "当前没有可重试的图片", "没有照片");
            notify_ui = true;
        }
        break;
    case EPHOTO_CMD_PURGE_CACHE:
        clear_pending_rebuild_photo_locked();
        display_service_purge_all_caches();
        begin_full_cache_build_locked(true, s_state.current_photo_name);
        set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "已清理图片缓存，准备重建", "缓存已清理");
        notify_ui = true;
        notify_photo = true;
        break;
    case EPHOTO_CMD_BUILD_MISSING_CACHE:
        clear_pending_rebuild_photo_locked();
        s_media_rescan_requested = true;
        s_cache_build_requested = true;
        s_cache_build_request_locks_interaction = true;
        s_cache_build_request_upload_batch = false;
        arm_cache_build_locked(true, false, command->text[0] ? command->text : s_state.current_photo_name);
        set_notification_locked(EPHOTO_NOTIFICATION_INFO,
                                command->text[0] ? command->text : "正在生成图片缓存",
                                "建立缓存");
        notify_ui = true;
        notify_photo = true;
        break;
    case EPHOTO_CMD_SHOW_NOTIFICATION:
        set_notification_locked(command->level,
                                command->text,
                                command->text);
        notify_ui = true;
        break;
    case EPHOTO_CMD_RESCAN_MEDIA:
        clear_pending_selected_photo_locked();
        clear_pending_rebuild_photo_locked();
        s_media_rescan_requested = true;
        notify_ui = true;
        notify_photo = true;
        break;
    case EPHOTO_CMD_QUEUE_UPLOADED_PHOTO:
        if (command->path[0]) {
            queue_pending_uploaded_photo_locked(command->path);
        }
        break;
    case EPHOTO_CMD_FINALIZE_UPLOAD_BATCH:
        clear_pending_selected_photo_locked();
        clear_pending_rebuild_photo_locked();
        if (command->path[0]) {
            s_focus_latest_uploaded_after_cache = true;
            strlcpy(s_pending_uploaded_photo_path, command->path, sizeof(s_pending_uploaded_photo_path));
        }
        s_media_rescan_requested = true;
        s_cache_build_requested = true;
        s_cache_build_request_locks_interaction = false;
        s_cache_build_request_upload_batch = true;
        set_notification_locked(EPHOTO_NOTIFICATION_INFO,
                                command->text[0] ? command->text : "正在为新上传照片生成缓存",
                                "建立缓存");
        notify_ui = true;
        notify_photo = true;
        break;
    default:
        break;
    }

    if (save_settings) {
        apply_settings_locked(true);
        if (s_state.menu_visible) {
            sync_menu_snapshot_locked();
        }
        if (s_album_visible) {
            album_sync_snapshot_locked();
        }
    }

    bump_state_version_locked();
    xSemaphoreGive(s_state_mutex);

    if (notify_ui) {
        notify_ui_task();
    }
    if (notify_photo) {
        notify_photo_task();
    }
}

static void control_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "control_task started on CPU%d", (int)xPortGetCoreID());
    ephoto_command_t command;
    while (xQueueReceive(s_command_queue, &command, portMAX_DELAY) == pdTRUE) {
        process_command(&command);
    }
}

static void media_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "media_task started on CPU%d", (int)xPortGetCoreID());
    ephoto_app_state_t *snapshot = allocate_task_snapshot("media_task");
    if (!snapshot) {
        vTaskDelete(NULL);
        return;
    }
    int64_t last_advance_ms = clock_service_now_ms();
    int64_t last_rescan_ms = clock_service_now_ms();
    int last_photo_index = -2;
    char last_photo_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    ephoto_slideshow_interval_t last_interval = (ephoto_slideshow_interval_t)-1;
    ephoto_fit_mode_t last_fit_mode = (ephoto_fit_mode_t)-1;
    uint16_t last_rotation_deg = UINT16_MAX;
    bool last_photo_ready = false;
    bool last_interaction_locked = true;
    bool last_screen_on = true;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        app_controller_snapshot(snapshot);
        int64_t now_ms = clock_service_now_ms();
        uint64_t interval_ms = ephoto_interval_to_ms(snapshot->settings.slideshow_interval);
        bool display_busy = display_service_is_busy();
        int pending_selected_index = -1;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        pending_selected_index = s_pending_selected_photo_index;
        xSemaphoreGive(s_state_mutex);
        bool current_photo_ready = !snapshot->current_photo_path[0] ||
                                   display_service_is_photo_ready(snapshot->current_photo_path,
                                                                  snapshot->settings.fit_mode,
                                                                  snapshot->settings.rotation_deg);

        bool photo_changed = snapshot->current_photo_index != last_photo_index ||
                             strcmp(snapshot->current_photo_path, last_photo_path) != 0;
        bool interval_changed = snapshot->settings.slideshow_interval != last_interval;
        bool view_changed = snapshot->settings.fit_mode != last_fit_mode ||
                            snapshot->settings.rotation_deg != last_rotation_deg;
        bool photo_became_ready = !last_photo_ready && current_photo_ready;
        bool interaction_unlocked = last_interaction_locked && !snapshot->interaction_locked;
        bool screen_state_changed = snapshot->settings.screen_on != last_screen_on;
        if (photo_changed || interval_changed || photo_became_ready || interaction_unlocked ||
            screen_state_changed || view_changed) {
            last_advance_ms = now_ms;
            last_photo_index = snapshot->current_photo_index;
            strlcpy(last_photo_path, snapshot->current_photo_path, sizeof(last_photo_path));
            last_interval = snapshot->settings.slideshow_interval;
            last_fit_mode = snapshot->settings.fit_mode;
            last_rotation_deg = snapshot->settings.rotation_deg;
        }
        last_photo_ready = current_photo_ready;
        last_interaction_locked = snapshot->interaction_locked;
        last_screen_on = snapshot->settings.screen_on;

        if (snapshot->menu_visible && now_ms - snapshot->menu_last_input_ms > 30000) {
            ephoto_command_t close_menu = {
                .type = EPHOTO_CMD_MENU_CLOSE,
            };
            strlcpy(close_menu.text, "菜单已超时退出", sizeof(close_menu.text));
            app_controller_submit(&close_menu);
        }

        if (snapshot->album.visible && now_ms - snapshot->menu_last_input_ms > 30000) {
            ephoto_command_t close_album = {
                .type = EPHOTO_CMD_MENU_CLOSE,
            };
            strlcpy(close_album.text, "相册已超时退出", sizeof(close_album.text));
            app_controller_submit(&close_album);
        }

        if (!snapshot->interaction_locked &&
            snapshot->settings.screen_on &&
            !snapshot->menu_visible &&
            !snapshot->album.visible &&
            snapshot->storage.photo_count > 0 &&
            pending_selected_index < 0 &&
            !display_busy &&
            current_photo_ready &&
            interval_ms > 0 &&
            now_ms - last_advance_ms >= (int64_t)interval_ms) {
            ephoto_command_t next = {.type = EPHOTO_CMD_NEXT};
            app_controller_submit(&next);
            last_advance_ms = now_ms;
        }

        if (!display_busy && now_ms - last_rescan_ms >= 300000) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_media_rescan_requested = true;
            xSemaphoreGive(s_state_mutex);
            last_rescan_ms = now_ms;
        }

        bool should_rescan = false;
        bool should_start_cache_build = false;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_media_rescan_requested) {
            s_media_rescan_requested = false;
            should_rescan = true;
        }
        if (s_cache_build_requested) {
            should_start_cache_build = true;
        }
        xSemaphoreGive(s_state_mutex);

        if (!should_rescan) {
            continue;
        }

        uint32_t revision_before = gallery_service_get_revision();
        gallery_service_rescan();
        uint32_t revision_after = gallery_service_get_revision();
        bool gallery_changed = revision_after != revision_before;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        sync_storage_locked();
        if (s_state.storage.photo_count > 0) {
            if (s_state.current_photo_index < 0) {
                s_state.current_photo_index = choose_photo_index_locked(-1, 1);
            }
            sync_current_photo_locked();
            if (s_state.current_photo_index < 0) {
                s_state.current_photo_index = choose_photo_index_locked(-1, 1);
                sync_current_photo_locked();
            }
        } else {
            s_state.current_photo_index = -1;
            s_state.current_photo_name[0] = '\0';
            s_state.current_photo_path[0] = '\0';
            s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
            set_cache_build_state_locked(false, false, 0, 0, 0, 0, 0, "");
        }
        if (s_state.menu_visible) {
            sync_menu_snapshot_locked();
        }
        if (s_album_visible) {
            album_sync_snapshot_locked();
        }
        bump_state_version_locked();
        xSemaphoreGive(s_state_mutex);

        if (gallery_changed) {
            notify_photo_task();
        }

        if (should_start_cache_build) {
            bool request_upload_batch = false;
            bool request_lock_interaction = false;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_cache_build_requested = false;
            request_upload_batch = s_cache_build_request_upload_batch;
            request_lock_interaction = s_cache_build_request_locks_interaction;
            s_cache_build_request_upload_batch = false;
            s_cache_build_request_locks_interaction = false;
            if (request_upload_batch && s_pending_uploaded_photo_count > 0) {
                begin_pending_upload_cache_build_locked(s_state.current_photo_name);
            } else {
                begin_full_cache_build_locked(request_lock_interaction, s_state.current_photo_name);
            }
            if (!s_state.cache_build_active && s_focus_latest_uploaded_after_cache && focus_latest_uploaded_photo_locked()) {
                s_focus_latest_uploaded_after_cache = false;
                set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "已切换到最近上传的照片", "最近上传");
            }
            bump_state_version_locked();
            xSemaphoreGive(s_state_mutex);
            notify_photo_task();
        }
        notify_ui_task();
    }
}

static void photo_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "photo_task started on CPU%d", (int)xPortGetCoreID());
    ephoto_app_state_t *snapshot = allocate_task_snapshot("photo_task");
    if (!snapshot) {
        vTaskDelete(NULL);
        return;
    }
    bool run_again_immediately = false;
    bool startup_initialized = false;
    int startup_photo_index = 0;
    size_t startup_combo_index = 0;
    uint32_t startup_total = 0;
    uint32_t startup_done = 0;
    uint32_t startup_failed = 0;
    uint32_t startup_photo_total = 0;
    uint32_t startup_photo_done = 0;
    bool startup_photo_has_pending = false;
    bool upload_batch_initialized = false;
    size_t upload_batch_index = 0;
    uint32_t upload_batch_total = 0;
    uint32_t upload_batch_done = 0;
    uint32_t upload_batch_failed = 0;
    uint32_t upload_batch_photo_total = 0;
    uint32_t upload_batch_photo_done = 0;

    while (true) {
        ulTaskNotifyTake(pdTRUE, run_again_immediately ? 0 : pdMS_TO_TICKS(300));
        run_again_immediately = false;

        app_controller_snapshot(snapshot);
        bool did_work = false;

        if (snapshot->cache_build_active) {
            int pending_rebuild_index = -1;
            ephoto_cache_build_mode_t cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            pending_rebuild_index = s_pending_rebuild_photo_index;
            cache_build_mode = s_cache_build_mode;
            xSemaphoreGive(s_state_mutex);

            if (pending_rebuild_index >= 0) {
                ephoto_photo_t rebuild_photo = {0};
                if (gallery_service_get_item(pending_rebuild_index, &rebuild_photo) != ESP_OK) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    if (s_pending_rebuild_photo_index == pending_rebuild_index) {
                        clear_pending_rebuild_photo_locked();
                        s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
                        set_cache_build_state_locked(false, false, 0, 0, 0, 0, 0, "");
                        set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "要重建的照片已不存在", "照片错误");
                        bump_state_version_locked();
                    }
                    xSemaphoreGive(s_state_mutex);
                    notify_ui_task();
                    continue;
                }
                uint32_t rebuild_done = 0;
                uint32_t rebuild_failed = 0;
                cache_progress_context_t progress = {
                    .base_done = 0,
                    .base_failed = 0,
                    .total = 6,
                    .photo_total = 1,
                    .photo_index = 1,
                    .lock_interaction = false,
                };
                strlcpy(progress.photo_name, rebuild_photo.name, sizeof(progress.photo_name));
                esp_err_t rebuild_err = display_service_prepare_missing_photo_caches(rebuild_photo.path,
                                                                                     &rebuild_done,
                                                                                     &rebuild_failed,
                                                                                     cache_build_progress_callback,
                                                                                     &progress);
                if (rebuild_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "explicit rebuild failed for %s: %s (done=%" PRIu32 " failed=%" PRIu32 ")",
                             rebuild_photo.path,
                             esp_err_to_name(rebuild_err),
                             rebuild_done,
                             rebuild_failed);
                }

                if (snapshot->current_photo_path[0] &&
                    strcmp(snapshot->current_photo_path, rebuild_photo.path) == 0 &&
                    display_service_has_cache(rebuild_photo.path,
                                              snapshot->settings.fit_mode,
                                              snapshot->settings.rotation_deg)) {
                    (void)display_service_stage_photo(rebuild_photo.path,
                                                      snapshot->settings.fit_mode,
                                                      snapshot->settings.rotation_deg);
                }

                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (s_pending_rebuild_photo_index == pending_rebuild_index) {
                    clear_pending_rebuild_photo_locked();
                    s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
                    set_cache_build_state_locked(false, false, 6, rebuild_done, rebuild_failed, 1, 1, "");
                    set_notification_locked(rebuild_failed == 0 ? EPHOTO_NOTIFICATION_SUCCESS : EPHOTO_NOTIFICATION_ERROR,
                                            rebuild_failed == 0 ? "该照片缓存已重建完成" : "该照片部分缓存重建失败",
                                            rebuild_failed == 0 ? "缓存完成" : "缓存失败");
                    bump_state_version_locked();
                }
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                continue;
            }

            if (cache_build_mode == EPHOTO_CACHE_BUILD_UPLOAD_BATCH) {
                startup_initialized = false;

                if (!upload_batch_initialized) {
                    upload_batch_index = 0;
                    upload_batch_done = 0;
                    upload_batch_failed = 0;
                    upload_batch_total = snapshot->cache_build_total;
                    upload_batch_photo_total = snapshot->cache_build_photo_total;
                    upload_batch_photo_done = 0;
                    upload_batch_initialized = true;
                }

                if (upload_batch_total == 0 || upload_batch_photo_total == 0 || s_pending_uploaded_photo_count == 0) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    clear_pending_uploaded_photo_queue_locked();
                    if (s_focus_latest_uploaded_after_cache && focus_latest_uploaded_photo_locked()) {
                        s_focus_latest_uploaded_after_cache = false;
                        set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "已切换到最近上传的照片", "最近上传");
                    }
                    s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
                    set_cache_build_state_locked(false, false, 0, 0, 0, 0, 0, "");
                    bump_state_version_locked();
                    xSemaphoreGive(s_state_mutex);
                    upload_batch_initialized = false;
                    notify_ui_task();
                    notify_photo_task();
                    continue;
                }

                if (upload_batch_index < s_pending_uploaded_photo_count) {
                    const char *path = s_pending_uploaded_photo_paths[upload_batch_index];
                    if (path[0]) {
                        bool has_pending = false;
                        uint32_t pending_jobs = count_missing_cache_jobs_for_path_locked(path, &has_pending);
                        if (has_pending && pending_jobs > 0) {
                            uint32_t completed = 0;
                            uint32_t failed = 0;
                            cache_progress_context_t progress = {
                                .base_done = upload_batch_done,
                                .base_failed = upload_batch_failed,
                                .total = upload_batch_total,
                                .photo_total = upload_batch_photo_total,
                                .photo_index = upload_batch_photo_done + 1U,
                                .lock_interaction = false,
                            };
                            const char *name = strrchr(path, '/');
                            name = name ? name + 1 : path;
                            strlcpy(progress.photo_name, name, sizeof(progress.photo_name));
                            esp_err_t cache_err = display_service_prepare_missing_photo_caches(path,
                                                                                               &completed,
                                                                                               &failed,
                                                                                               cache_build_progress_callback,
                                                                                               &progress);
                            upload_batch_done += completed;
                            upload_batch_failed += failed;
                            upload_batch_photo_done += 1U;
                            if (cache_err != ESP_OK) {
                                ESP_LOGW(TAG,
                                         "upload cache build failed for %s: %s (done=%" PRIu32 " failed=%" PRIu32 ")",
                                         path,
                                         esp_err_to_name(cache_err),
                                         completed,
                                         failed);
                            }

                            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                            if (upload_batch_index < s_pending_uploaded_photo_count) {
                                set_cache_build_state_locked(true,
                                                             false,
                                                             upload_batch_total,
                                                             upload_batch_done,
                                                             upload_batch_failed,
                                                             upload_batch_photo_total,
                                                             upload_batch_photo_done,
                                                             name);
                                bump_state_version_locked();
                            }
                            xSemaphoreGive(s_state_mutex);
                            notify_ui_task();
                        }
                    }

                    upload_batch_index += 1U;
                    run_again_immediately = true;
                    continue;
                }

                upload_batch_initialized = false;
                char target_photo_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
                ephoto_fit_mode_t target_fit_mode = snapshot->settings.fit_mode;
                uint16_t target_rotation_deg = snapshot->settings.rotation_deg;
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                clear_pending_uploaded_photo_queue_locked();
                if (s_focus_latest_uploaded_after_cache && focus_latest_uploaded_photo_locked()) {
                    s_focus_latest_uploaded_after_cache = false;
                    set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "已切换到最近上传的照片", "最近上传");
                } else {
                    set_notification_locked(upload_batch_failed == 0 ? EPHOTO_NOTIFICATION_SUCCESS : EPHOTO_NOTIFICATION_ERROR,
                                            upload_batch_failed == 0 ? "新上传照片缓存已生成完成" : "部分新上传照片缓存生成失败",
                                            upload_batch_failed == 0 ? "缓存完成" : "缓存失败");
                }
                strlcpy(target_photo_path, s_state.current_photo_path, sizeof(target_photo_path));
                s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
                set_cache_build_state_locked(false, false, upload_batch_total, upload_batch_done, upload_batch_failed, 0, 0, "");
                bump_state_version_locked();
                xSemaphoreGive(s_state_mutex);

                if (target_photo_path[0] &&
                    display_service_has_cache(target_photo_path, target_fit_mode, target_rotation_deg)) {
                    (void)display_service_stage_photo(target_photo_path, target_fit_mode, target_rotation_deg);
                }

                notify_ui_task();
                notify_photo_task();
                continue;
            }

            if (snapshot->cache_build_total == 0) {
                startup_initialized = false;
                upload_batch_initialized = false;
                continue;
            }

            int photo_count = gallery_service_get_count();
            if (!startup_initialized) {
                startup_photo_index = 0;
                startup_combo_index = 0;
                startup_done = 0;
                startup_failed = 0;
                startup_photo_total = snapshot->cache_build_photo_total;
                startup_photo_done = 0;
                startup_photo_has_pending = false;
                startup_total = snapshot->cache_build_total;
                startup_initialized = true;
            }

            if (startup_total == 0 || photo_count <= 0) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (s_focus_latest_uploaded_after_cache && focus_latest_uploaded_photo_locked()) {
                    s_focus_latest_uploaded_after_cache = false;
                    set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "已切换到最近上传的照片", "最近上传");
                }
                s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
                set_cache_build_state_locked(false, false, 0, 0, 0, 0, 0, "");
                bump_state_version_locked();
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                notify_photo_task();
                continue;
            }

            if (startup_photo_index < photo_count) {
                ephoto_photo_t photo = {0};
                if (gallery_service_get_item(startup_photo_index, &photo) == ESP_OK) {
                    if (startup_combo_index == 0) {
                        uint32_t completed = 0;
                        uint32_t failed = 0;
                        cache_progress_context_t progress = {
                            .base_done = startup_done,
                            .base_failed = startup_failed,
                            .total = startup_total,
                            .photo_total = startup_photo_total,
                            .photo_index = startup_photo_done + 1U,
                            .lock_interaction = true,
                        };
                        strlcpy(progress.photo_name, photo.name, sizeof(progress.photo_name));
                        esp_err_t cache_err = display_service_prepare_missing_photo_caches(photo.path,
                                                                                           &completed,
                                                                                           &failed,
                                                                                           cache_build_progress_callback,
                                                                                           &progress);
                        startup_photo_has_pending = completed > 0;
                        startup_done += completed;
                        startup_failed += failed;
                        if (cache_err != ESP_OK) {
                            ESP_LOGW(TAG,
                                     "startup cache build failed for %s: %s (done=%" PRIu32 " failed=%" PRIu32 ")",
                                     photo.path,
                                     esp_err_to_name(cache_err),
                                     completed,
                                     failed);
                        }

                        if (startup_photo_has_pending) {
                            ++startup_photo_done;
                            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                            set_cache_build_state_locked(true,
                                                         true,
                                                         startup_total,
                                                         startup_done,
                                                         startup_failed,
                                                         startup_photo_total,
                                                         startup_photo_done,
                                                         photo.name);
                            bump_state_version_locked();
                            xSemaphoreGive(s_state_mutex);
                            notify_ui_task();
                        }

                        startup_photo_has_pending = false;
                        startup_combo_index = 0;
                        ++startup_photo_index;
                    }
                    run_again_immediately = true;
                    continue;
                }

                ++startup_failed;
                ++startup_done;
                if (startup_photo_has_pending) {
                    ++startup_photo_done;
                }
                startup_photo_has_pending = false;
                ++startup_photo_index;
                run_again_immediately = true;
                continue;
            }

            startup_initialized = false;
            char target_photo_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
            ephoto_fit_mode_t target_fit_mode = snapshot->settings.fit_mode;
            uint16_t target_rotation_deg = snapshot->settings.rotation_deg;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (s_focus_latest_uploaded_after_cache && focus_latest_uploaded_photo_locked()) {
                s_focus_latest_uploaded_after_cache = false;
                set_notification_locked(EPHOTO_NOTIFICATION_SUCCESS, "已切换到最近上传的照片", "最近上传");
            }
            strlcpy(target_photo_path, s_state.current_photo_path, sizeof(target_photo_path));
            xSemaphoreGive(s_state_mutex);
            if (target_photo_path[0]) {
                esp_err_t stage_err = display_service_stage_photo(target_photo_path,
                                                                  target_fit_mode,
                                                                  target_rotation_deg);
                if (stage_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "stage photo after cache build failed for %s: %s, retry current-photo rebuild",
                             target_photo_path,
                             esp_err_to_name(stage_err));
                    display_service_invalidate_photo_cache(target_photo_path);
                    esp_err_t prepare_err = display_service_prepare_photo_cache(target_photo_path,
                                                                                target_fit_mode,
                                                                                target_rotation_deg);
                    if (prepare_err == ESP_OK) {
                        stage_err = display_service_stage_photo(target_photo_path,
                                                                target_fit_mode,
                                                                target_rotation_deg);
                    }
                    if (stage_err != ESP_OK) {
                        ESP_LOGW(TAG,
                                 "retry stage after cache build still failed for %s: %s",
                                 target_photo_path,
                                 esp_err_to_name(stage_err));
                    }
                }
            }

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_cache_build_mode = EPHOTO_CACHE_BUILD_NONE;
            set_cache_build_state_locked(false, false, startup_total, startup_done, startup_failed, 0, 0, "");
            bump_state_version_locked();
            xSemaphoreGive(s_state_mutex);
            notify_ui_task();
            continue;
        }

        startup_initialized = false;
        upload_batch_initialized = false;

        int pending_selected_index = -1;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        pending_selected_index = s_pending_selected_photo_index;
        xSemaphoreGive(s_state_mutex);

        if (pending_selected_index >= 0) {
            if (pending_selected_index == snapshot->current_photo_index &&
                snapshot->current_photo_path[0] &&
                display_service_is_photo_ready(snapshot->current_photo_path,
                                               snapshot->settings.fit_mode,
                                               snapshot->settings.rotation_deg)) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                clear_pending_selected_photo_locked();
                clear_photo_transition_notification_locked();
                set_notification_locked(EPHOTO_NOTIFICATION_INFO, "当前照片已在播放", "保持当前");
                bump_state_version_locked();
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                continue;
            }

            ephoto_photo_t requested = {0};
            esp_err_t get_err = gallery_service_get_item(pending_selected_index, &requested);
            if (get_err != ESP_OK) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                clear_pending_selected_photo_locked();
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "未找到要显示的照片", "照片错误");
                bump_state_version_locked();
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                continue;
            }
            if (!photo_matches_orientation_filter(&requested, snapshot->settings.orientation_filter)) {
                char detail[EPHOTO_MAX_NOTIFICATION_LEN];
                orientation_filter_block_message(&requested,
                                                 snapshot->settings.orientation_filter,
                                                 detail,
                                                 sizeof(detail));
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                clear_pending_selected_photo_locked();
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, detail, "方向不符");
                bump_state_version_locked();
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                continue;
            }

            bool cache_exists = display_service_has_cache(requested.path,
                                                          snapshot->settings.fit_mode,
                                                          snapshot->settings.rotation_deg);
            bool cache_failed = !cache_exists &&
                                display_service_has_known_cache_failure(requested.path,
                                                                        snapshot->settings.fit_mode,
                                                                        snapshot->settings.rotation_deg);
            if (cache_failed) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                clear_pending_selected_photo_locked();
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "所选照片缓存曾构建失败，请手动重建", "照片错误");
                bump_state_version_locked();
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                continue;
            }
            if (!cache_exists) {
                esp_err_t prepare_err = display_service_prepare_photo_cache(requested.path,
                                                                            snapshot->settings.fit_mode,
                                                                            snapshot->settings.rotation_deg);
                if (prepare_err == ESP_OK) {
                    notify_ui_task();
                    run_again_immediately = true;
                    continue;
                }

                ESP_LOGW(TAG,
                         "prepare selected photo cache failed for %s: %s",
                         requested.path,
                         esp_err_to_name(prepare_err));
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                clear_pending_selected_photo_locked();
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "所选照片缓存生成失败", "照片错误");
                bump_state_version_locked();
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                continue;
            }

            esp_err_t stage_err = display_service_stage_photo(requested.path,
                                                              snapshot->settings.fit_mode,
                                                              snapshot->settings.rotation_deg);
            if (stage_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "stage selected photo failed for %s: %s",
                         requested.path,
                         esp_err_to_name(stage_err));
                display_service_invalidate_photo_cache(requested.path);
                esp_err_t prepare_err = display_service_prepare_photo_cache(requested.path,
                                                                            snapshot->settings.fit_mode,
                                                                            snapshot->settings.rotation_deg);
                if (prepare_err == ESP_OK) {
                    notify_ui_task();
                    run_again_immediately = true;
                    continue;
                }

                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                clear_pending_selected_photo_locked();
                set_notification_locked(EPHOTO_NOTIFICATION_ERROR, "所选照片暂时无法显示", "照片错误");
                bump_state_version_locked();
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                continue;
            }

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (s_pending_selected_photo_index == pending_selected_index) {
                s_state.current_photo_index = pending_selected_index;
                sync_current_photo_locked();
                clear_pending_selected_photo_locked();
                clear_photo_transition_notification_locked();
                bump_state_version_locked();
            }
            xSemaphoreGive(s_state_mutex);
            notify_ui_task();
            continue;
        }

        if (snapshot->current_photo_path[0] &&
            !display_service_is_photo_ready(snapshot->current_photo_path,
                                            snapshot->settings.fit_mode,
                                            snapshot->settings.rotation_deg)) {
            bool cache_exists = display_service_has_cache(snapshot->current_photo_path,
                                                          snapshot->settings.fit_mode,
                                                          snapshot->settings.rotation_deg);
            bool cache_failed = !cache_exists &&
                                display_service_has_known_cache_failure(snapshot->current_photo_path,
                                                                        snapshot->settings.fit_mode,
                                                                        snapshot->settings.rotation_deg);
            if (cache_failed) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (clear_photo_transition_notification_locked()) {
                    bump_state_version_locked();
                }
                xSemaphoreGive(s_state_mutex);
                notify_ui_task();
                continue;
            }
            if (!cache_exists) {
                ESP_LOGW(TAG,
                         "display cache missing for current photo %s, preparing current view only",
                         snapshot->current_photo_path);
                esp_err_t prepare_err = display_service_prepare_photo_cache(snapshot->current_photo_path,
                                                                            snapshot->settings.fit_mode,
                                                                            snapshot->settings.rotation_deg);
                if (prepare_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "prepare current photo cache failed for %s: %s",
                             snapshot->current_photo_path,
                             esp_err_to_name(prepare_err));
                } else {
                    notify_ui_task();
                    run_again_immediately = true;
                    continue;
                }
            }

            esp_err_t stage_err = display_service_stage_photo(snapshot->current_photo_path,
                                                              snapshot->settings.fit_mode,
                                                              snapshot->settings.rotation_deg);
            if (stage_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "stage current photo failed for %s: %s, rebuilding current cache only",
                         snapshot->current_photo_path,
                         esp_err_to_name(stage_err));
                display_service_invalidate_photo_cache(snapshot->current_photo_path);
                esp_err_t prepare_err = display_service_prepare_photo_cache(snapshot->current_photo_path,
                                                                            snapshot->settings.fit_mode,
                                                                            snapshot->settings.rotation_deg);
                if (prepare_err == ESP_OK) {
                    notify_ui_task();
                    run_again_immediately = true;
                    continue;
                }
                ESP_LOGW(TAG,
                         "rebuild current photo cache failed for %s: %s",
                         snapshot->current_photo_path,
                         esp_err_to_name(prepare_err));
            } else {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (clear_photo_transition_notification_locked()) {
                    bump_state_version_locked();
                }
                xSemaphoreGive(s_state_mutex);
            }

            notify_ui_task();
            did_work = true;
        }

        if (did_work) {
            run_again_immediately = true;
        }
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ui_task started on CPU%d", (int)xPortGetCoreID());
    ephoto_app_state_t *snapshot = allocate_task_snapshot("ui_task");
    if (!snapshot) {
        vTaskDelete(NULL);
        return;
    }
    uint32_t last_state_version = UINT32_MAX;
    int64_t last_clock_minute = -1;
    bool last_notification_visible = false;
    bool last_cache_build_active = false;

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }

        app_controller_snapshot(snapshot);

        int64_t now_ms = clock_service_now_ms();
        bool clock_visible = !snapshot->cache_build_active &&
                             snapshot->settings.clock_visible &&
                             snapshot->settings.screen_on &&
                             !snapshot->menu_visible &&
                             !snapshot->album.visible;
        int64_t clock_minute = clock_visible ? (now_ms / 60000) : -1;
        bool notification_visible = snapshot->notification.updated_at_ms > 0 &&
                                    (now_ms - snapshot->notification.updated_at_ms) <= 3000;
        bool state_changed = snapshot->state_version != last_state_version;
        bool clock_tick_changed = clock_minute != last_clock_minute;
        bool notification_visibility_changed = notification_visible != last_notification_visible;
        bool cache_visibility_changed = snapshot->cache_build_active != last_cache_build_active;
        bool album_thumbnail_pending = snapshot->album.visible && display_service_has_pending_album_thumbnail_work();

        if (!state_changed &&
            !clock_tick_changed &&
            !notification_visibility_changed &&
            !cache_visibility_changed &&
            !album_thumbnail_pending) {
            continue;
        }

        display_service_render_state(snapshot);
        display_service_mark_frame_rendered();
        if (snapshot->album.visible && display_service_has_pending_album_thumbnail_work()) {
            notify_ui_task();
        }
        last_state_version = snapshot->state_version;
        last_clock_minute = clock_minute;
        last_notification_visible = notification_visible;
        last_cache_build_active = snapshot->cache_build_active;
    }
}

static void settings_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "settings_task started on CPU%d", (int)xPortGetCoreID());
    int64_t last_schedule_check_ms = 0;
    int64_t last_ambient_poll_ms = 0;
    int64_t last_ota_tick_ms = 0;
    int64_t last_rotation_poll_ms = 0;
    int64_t last_hosted_refresh_ms = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250));
        clock_service_poll(wifi_admin_is_sta_connected());

        ephoto_settings_t settings_snapshot;
        bool should_save = false;
        bool should_notify_ui = false;
        bool should_notify_photo = false;
        int64_t now_ms = clock_service_now_ms();

        if ((now_ms - last_hosted_refresh_ms) >= 2000) {
            esp_err_t hosted_err = hosted_service_refresh(false);
            if (hosted_err == ESP_OK && hosted_service_is_ready()) {
                esp_err_t ota_err = hosted_service_upgrade_embedded_if_needed();
                if (ota_err != ESP_OK) {
                    ESP_LOGW(TAG, "background hosted OTA skipped/failed: %s", esp_err_to_name(ota_err));
                }
            }
            last_hosted_refresh_ms = now_ms;
        }

        if ((now_ms - last_ambient_poll_ms) >= 1000) {
            ambient_light_status_t light_status = {0};
            esp_err_t light_err = ambient_light_service_poll(&light_status);
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (light_err == ESP_OK) {
                uint16_t prev_mv = s_state.ambient_light_raw_mv;
                uint8_t prev_percent = s_state.ambient_light_percent;
                ephoto_brightness_t prev_brightness = s_state.settings.brightness;
                sync_ambient_light_locked(&light_status);
                if (s_state.settings.brightness_mode == EPHOTO_BRIGHTNESS_MODE_AUTO) {
                    ephoto_brightness_t next_brightness = auto_brightness_from_percent(light_status.percent);
                    if (abs((int)prev_brightness - (int)next_brightness) >= 2 ||
                        prev_mv != s_state.ambient_light_raw_mv ||
                        prev_percent != s_state.ambient_light_percent) {
                        s_state.settings.brightness = next_brightness;
                        bump_state_version_locked();
                        should_notify_ui = true;
                    }
                } else if (prev_mv != s_state.ambient_light_raw_mv ||
                           prev_percent != s_state.ambient_light_percent) {
                    bump_state_version_locked();
                    should_notify_ui = true;
                }
            }
            xSemaphoreGive(s_state_mutex);
            last_ambient_poll_ms = now_ms;
        }

        if ((now_ms - last_schedule_check_ms) >= 1000) {
            struct tm tm_now = {0};
            if (clock_service_get_local_tm(&tm_now)) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (s_state.settings.screen_schedule_enabled) {
                    bool schedule_screen_on = schedule_should_turn_screen_on(&s_state.settings, &tm_now);
                    if (s_state.settings.screen_on != schedule_screen_on) {
                        s_state.settings.screen_on = schedule_screen_on;
                        bump_state_version_locked();
                        should_notify_ui = true;
                    }
                }
                xSemaphoreGive(s_state_mutex);
            }
            last_schedule_check_ms = now_ms;
        }

        if ((now_ms - last_rotation_poll_ms) >= 250) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (s_state.settings.auto_rotation_enabled && refresh_effective_rotation_locked()) {
                bump_state_version_locked();
                should_notify_ui = true;
                should_notify_photo = true;
            }
            xSemaphoreGive(s_state_mutex);
            last_rotation_poll_ms = now_ms;
        }

        if ((now_ms - last_ota_tick_ms) >= 1000) {
            ephoto_settings_t ota_settings = {0};
            bool sta_connected = wifi_admin_is_sta_connected();
            bool ota_runtime_busy = false;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            ota_settings = s_state.settings;
            ota_runtime_busy = s_state.cache_build_active;
            xSemaphoreGive(s_state_mutex);
            ota_service_set_runtime_busy(ota_runtime_busy);
            ota_service_tick(&ota_settings, sta_connected);
            last_ota_tick_ms = now_ms;
        }

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        ephoto_ota_status_t prev_ota = s_state.ota;
        ephoto_boot_image_status_t prev_boot_image = s_state.boot_image;
        ephoto_network_mode_t prev_network_mode = s_state.network_mode;
        char prev_ssid[EPHOTO_MAX_SSID_LEN];
        strlcpy(prev_ssid, s_state.current_ssid, sizeof(prev_ssid));
        sync_network_locked();
        if (prev_network_mode != s_state.network_mode ||
            strcmp(prev_ssid, s_state.current_ssid) != 0) {
            bump_state_version_locked();
            should_notify_ui = true;
        }
        sync_boot_image_locked();
        if (memcmp(&prev_boot_image, &s_state.boot_image, sizeof(prev_boot_image)) != 0) {
            bump_state_version_locked();
            should_notify_ui = true;
        }
        sync_hosted_locked();
        if (sync_ota_locked()) {
            notify_ota_transition_locked(&prev_ota, &s_state.ota);
            bump_state_version_locked();
            should_notify_ui = true;
        }
        if (s_settings_dirty && (clock_service_now_ms() - s_settings_dirty_at_ms) >= 800) {
            settings_snapshot = s_state.settings;
            s_settings_dirty = false;
            should_save = true;
        }
        xSemaphoreGive(s_state_mutex);

        if (should_save) {
            if (settings_snapshot.screen_schedule_enabled) {
                settings_snapshot.screen_on = true;
            }
            esp_err_t err = settings_service_save(&settings_snapshot);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "deferred settings save failed: %s", esp_err_to_name(err));
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_settings_dirty = true;
                s_settings_dirty_at_ms = clock_service_now_ms();
                xSemaphoreGive(s_state_mutex);
            }
        }

        if (should_notify_ui) {
            notify_ui_task();
        }
        if (should_notify_photo) {
            notify_photo_task();
        }
    }
}

esp_err_t app_controller_init(const board_profile_t *profile)
{
    s_profile = profile;
    s_command_queue = xQueueCreate(64, sizeof(ephoto_command_t));
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_command_queue || !s_state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_menu_selected_index = 0;
    s_album_visible = false;
    s_album_action_popup_visible = false;
    s_album_action_index = 0;
    s_album_action_confirm_pending = false;
    s_album_action_confirm_index = 0;
    s_album_selected_index = -1;
    for (size_t i = 0; i < EPHOTO_ALBUM_MAX_SLOTS; ++i) {
        s_album_slot_photo_indices[i] = -1;
        s_album_slot_thumb_ready[i] = false;
    }

    s_state.current_photo_index = -1;
    settings_service_load(&s_state.settings);
    s_state.settings.manual_rotation_deg = s_state.settings.manual_rotation_deg == 90 ? 90 : 0;
    s_state.settings.rotation_deg = s_state.settings.rotation_deg == 90 ? 90 : 0;
    (void)refresh_effective_rotation_locked();
    s_state.settings.manual_brightness = normalize_brightness(s_state.settings.manual_brightness);
    if (s_state.settings.brightness_mode == EPHOTO_BRIGHTNESS_MODE_MANUAL) {
        s_state.settings.brightness = s_state.settings.manual_brightness;
    }
    ambient_light_service_init(profile);
    ambient_light_status_t light_status = {0};
    if (ambient_light_service_poll(&light_status) == ESP_OK) {
        sync_ambient_light_locked(&light_status);
        if (s_state.settings.brightness_mode == EPHOTO_BRIGHTNESS_MODE_AUTO) {
            s_state.settings.brightness = auto_brightness_from_percent(light_status.percent);
        }
    }
    clock_service_init(s_state.settings.timezone, s_state.settings.auto_time_sync);
    sync_storage_locked();
    sync_network_locked();
    sync_boot_image_locked();
    ota_service_tick(&s_state.settings, wifi_admin_is_sta_connected());
    sync_ota_locked();
    sync_hosted_locked();
    if (s_state.storage.photo_count > 0) {
        s_state.current_photo_index = choose_photo_index_locked(-1, 1);
    }
    sync_current_photo_locked();
    prime_startup_photo_if_possible();
    notification_service_snapshot(&s_state.notification);
    display_service_apply_settings(&s_state.settings);
    if (s_state.network_mode == EPHOTO_NETWORK_AP) {
        notify_wifi_access_locked("设备已启动");
    }
    bump_state_version_locked();

    ESP_LOGI(TAG, "controller initialized");
    return ESP_OK;
}

esp_err_t app_controller_start(void)
{
    xTaskCreatePinnedToCore(control_task, "control_task", EPHOTO_STACK_CONTROL, NULL, EPHOTO_PRIO_CONTROL, &s_control_task_handle, EPHOTO_CORE_INTERACTIVE);
    xTaskCreatePinnedToCore(media_task, "media_task", EPHOTO_STACK_MEDIA, NULL, EPHOTO_PRIO_MEDIA, &s_media_task_handle, EPHOTO_CORE_BACKGROUND);
    xTaskCreatePinnedToCore(photo_task, "photo_task", EPHOTO_STACK_PHOTO, NULL, EPHOTO_PRIO_PHOTO, &s_photo_task_handle, EPHOTO_CORE_BACKGROUND);
    xTaskCreatePinnedToCore(ui_task, "ui_task", EPHOTO_STACK_UI, NULL, EPHOTO_PRIO_UI, &s_ui_task_handle, EPHOTO_CORE_BACKGROUND);
    xTaskCreatePinnedToCore(settings_task, "settings_task", EPHOTO_STACK_SETTINGS, NULL, EPHOTO_PRIO_SETTINGS, &s_settings_task_handle, EPHOTO_CORE_BACKGROUND);
    notify_photo_task();
    notify_ui_task();
    return ESP_OK;
}

esp_err_t app_controller_submit(const ephoto_command_t *command)
{
    if (!command || !s_command_queue) {
        return ESP_ERR_INVALID_ARG;
    }
    return xQueueSend(s_command_queue, command, pdMS_TO_TICKS(200)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void app_controller_snapshot(ephoto_app_state_t *out_state)
{
    if (!out_state || !s_state_mutex) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out_state = s_state;
    xSemaphoreGive(s_state_mutex);
}
