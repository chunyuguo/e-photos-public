#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPHOTO_MAX_SSID_LEN            32
#define EPHOTO_MAX_PASSWORD_LEN        64
#define EPHOTO_MAX_WIFI_PROFILES       8
#define EPHOTO_MAX_NOTIFICATION_LEN    128
#define EPHOTO_MAX_PHOTO_NAME_LEN      256
#define EPHOTO_MAX_PHOTO_PATH_LEN      512
#define EPHOTO_MAX_ENCODED_NAME_LEN    1024
#define EPHOTO_MAX_TIMEZONE_LEN        32
#define EPHOTO_MAX_DEVICE_NAME_LEN     48
#define EPHOTO_MAX_HOSTNAME_LEN        32
#define EPHOTO_MAX_MOUNT_PATH_LEN      32
#define EPHOTO_MAX_PHOTO_DIR_LEN       48
#define EPHOTO_MAX_OTA_URL_LEN         256
#define EPHOTO_MAX_OTA_CHANNEL_LEN     24
#define EPHOTO_MAX_OTA_VERSION_LEN     48
#define EPHOTO_MAX_OTA_ARTIFACT_LEN    96
#define EPHOTO_MAX_OTA_PROVIDER_LEN    24
#define EPHOTO_MAX_OTA_DEVICE_TYPE_LEN 48
#define EPHOTO_MAX_OTA_DEPLOYMENT_LEN  64
#define EPHOTO_MAX_OTA_MESSAGE_LEN     128
#define EPHOTO_MAX_OTA_NOTES_LEN       2047
#define EPHOTO_MAX_BOOT_IMAGE_NAME_LEN 256
#define EPHOTO_BUTTON_COUNT            5
#define EPHOTO_MENU_MAX_DEPTH          4
#define EPHOTO_MENU_MAX_ITEMS          10
#define EPHOTO_MENU_LABEL_LEN          48
#define EPHOTO_ALBUM_MAX_SLOTS         15
#define EPHOTO_ALBUM_MAX_ACTIONS       6

typedef enum {
    EPHOTO_PLAYBACK_TIME_ASC = 0,
    EPHOTO_PLAYBACK_TIME_DESC,
    EPHOTO_PLAYBACK_RANDOM,
} ephoto_playback_mode_t;

typedef enum {
    EPHOTO_INTERVAL_OFF = 0,
    EPHOTO_INTERVAL_10S,
    EPHOTO_INTERVAL_1M,
    EPHOTO_INTERVAL_30M,
    EPHOTO_INTERVAL_60M,
    EPHOTO_INTERVAL_6H,
    EPHOTO_INTERVAL_12H,
    EPHOTO_INTERVAL_24H,
    EPHOTO_INTERVAL_1W,
    EPHOTO_INTERVAL_1MO,
} ephoto_slideshow_interval_t;

typedef uint8_t ephoto_brightness_t;

#define EPHOTO_BRIGHTNESS_MIN           ((ephoto_brightness_t)0)
#define EPHOTO_BRIGHTNESS_MAX           ((ephoto_brightness_t)100)
#define EPHOTO_BRIGHTNESS_PRESET_LOW    ((ephoto_brightness_t)25)
#define EPHOTO_BRIGHTNESS_PRESET_MEDIUM ((ephoto_brightness_t)60)
#define EPHOTO_BRIGHTNESS_PRESET_HIGH   ((ephoto_brightness_t)100)

typedef enum {
    EPHOTO_BRIGHTNESS_MODE_MANUAL = 0,
    EPHOTO_BRIGHTNESS_MODE_AUTO,
} ephoto_brightness_mode_t;

typedef enum {
    EPHOTO_CLOCK_FORMAT_24H = 0,
    EPHOTO_CLOCK_FORMAT_12H,
} ephoto_clock_format_t;

typedef enum {
    EPHOTO_CLOCK_POSITION_TOP_RIGHT = 0,
    EPHOTO_CLOCK_POSITION_TOP_LEFT,
    EPHOTO_CLOCK_POSITION_BOTTOM_LEFT,
    EPHOTO_CLOCK_POSITION_BOTTOM_RIGHT,
} ephoto_clock_position_t;

typedef enum {
    EPHOTO_CLOCK_COLOR_WHITE = 0,
    EPHOTO_CLOCK_COLOR_DARK_GRAY,
} ephoto_clock_color_t;

typedef enum {
    EPHOTO_OTA_INTERVAL_OFF = 0,
    EPHOTO_OTA_INTERVAL_6H,
    EPHOTO_OTA_INTERVAL_24H,
    EPHOTO_OTA_INTERVAL_1W,
} ephoto_ota_interval_t;

typedef enum {
    EPHOTO_OTA_STAGE_IDLE = 0,
    EPHOTO_OTA_STAGE_CHECKING,
    EPHOTO_OTA_STAGE_UP_TO_DATE,
    EPHOTO_OTA_STAGE_UPDATE_AVAILABLE,
    EPHOTO_OTA_STAGE_DOWNLOADING,
    EPHOTO_OTA_STAGE_APPLYING,
    EPHOTO_OTA_STAGE_RESTARTING,
    EPHOTO_OTA_STAGE_SUCCESS,
    EPHOTO_OTA_STAGE_ERROR,
} ephoto_ota_stage_t;

typedef enum {
    EPHOTO_FIT_CONTAIN = 0,
    EPHOTO_FIT_COVER,
} ephoto_fit_mode_t;

typedef enum {
    EPHOTO_ORIENTATION_ALL = 0,
    EPHOTO_ORIENTATION_LANDSCAPE,
    EPHOTO_ORIENTATION_PORTRAIT,
} ephoto_orientation_filter_t;

typedef enum {
    EPHOTO_NOTIFICATION_INFO = 0,
    EPHOTO_NOTIFICATION_SUCCESS,
    EPHOTO_NOTIFICATION_ERROR,
} ephoto_notification_level_t;

typedef enum {
    EPHOTO_BOOT_IMAGE_SOURCE_DEFAULT = 0,
    EPHOTO_BOOT_IMAGE_SOURCE_CUSTOM,
} ephoto_boot_image_source_t;

typedef enum {
    EPHOTO_NETWORK_UNAVAILABLE = 0,
    EPHOTO_NETWORK_DISCONNECTED,
    EPHOTO_NETWORK_AP,
    EPHOTO_NETWORK_STA,
} ephoto_network_mode_t;

typedef enum {
    EPHOTO_BUTTON_CONFIRM = 0,
    EPHOTO_BUTTON_PREV,
    EPHOTO_BUTTON_NEXT,
    EPHOTO_BUTTON_ROTATE,
    EPHOTO_BUTTON_ZOOM,
} ephoto_button_id_t;

typedef enum {
    EPHOTO_CMD_NONE = 0,
    EPHOTO_CMD_HW_CONFIRM,
    EPHOTO_CMD_HW_PREV,
    EPHOTO_CMD_HW_NEXT,
    EPHOTO_CMD_HW_ROTATE,
    EPHOTO_CMD_HW_ZOOM,
    EPHOTO_CMD_CONFIRM,
    EPHOTO_CMD_MENU_LONGPRESS,
    EPHOTO_CMD_MENU_CLOSE,
    EPHOTO_CMD_PREV,
    EPHOTO_CMD_NEXT,
    EPHOTO_CMD_SHOW_PHOTO_INDEX,
    EPHOTO_CMD_REBUILD_PHOTO_INDEX,
    EPHOTO_CMD_DELETE_PHOTO_INDEX,
    EPHOTO_CMD_DELETE_PHOTO_PATH,
    EPHOTO_CMD_SET_PHOTO_ROTATION_90_INDEX,
    EPHOTO_CMD_SET_PHOTO_ROTATION_180_INDEX,
    EPHOTO_CMD_SET_PHOTO_ROTATION_270_INDEX,
    EPHOTO_CMD_RESET_PHOTO_ORIENTATION_INDEX,
    EPHOTO_CMD_ROTATE,
    EPHOTO_CMD_FIT_TOGGLE,
    EPHOTO_CMD_SCREEN_TOGGLE,
    EPHOTO_CMD_BRIGHTNESS_STEP,
    EPHOTO_CMD_CLOCK_TOGGLE,
    EPHOTO_CMD_START_WIFI_AP,
    EPHOTO_CMD_CONNECT_WIFI,
    EPHOTO_CMD_SET_PLAYBACK_MODE,
    EPHOTO_CMD_SET_INTERVAL,
    EPHOTO_CMD_SET_ROTATION,
    EPHOTO_CMD_SET_AUTO_ROTATION,
    EPHOTO_CMD_SET_BRIGHTNESS,
    EPHOTO_CMD_SET_BRIGHTNESS_MODE,
    EPHOTO_CMD_SET_FIT_MODE,
    EPHOTO_CMD_SET_ORIENTATION_FILTER,
    EPHOTO_CMD_SET_SCREEN_ON,
    EPHOTO_CMD_SET_SCREEN_SCHEDULE_ENABLED,
    EPHOTO_CMD_SET_SCREEN_ON_MINUTE,
    EPHOTO_CMD_SET_SCREEN_OFF_MINUTE,
    EPHOTO_CMD_SET_CLOCK_VISIBLE,
    EPHOTO_CMD_SET_CLOCK_FORMAT,
    EPHOTO_CMD_SET_CLOCK_POSITION,
    EPHOTO_CMD_SET_CLOCK_COLOR,
    EPHOTO_CMD_SET_OTA_INTERVAL,
    EPHOTO_CMD_SET_OTA_CHANNEL,
    EPHOTO_CMD_OTA_CHECK_NOW,
    EPHOTO_CMD_OTA_UPDATE_NOW,
    EPHOTO_CMD_SET_TIMEZONE,
    EPHOTO_CMD_SET_AUTO_TIME_SYNC,
    EPHOTO_CMD_SYNC_TIME_NOW,
    EPHOTO_CMD_SET_MANUAL_TIME,
    EPHOTO_CMD_REFRESH_WIFI_STATE,
    EPHOTO_CMD_REPROCESS_CURRENT,
    EPHOTO_CMD_PURGE_CACHE,
    EPHOTO_CMD_BUILD_MISSING_CACHE,
    EPHOTO_CMD_SHOW_NOTIFICATION,
    EPHOTO_CMD_RESCAN_MEDIA,
    EPHOTO_CMD_QUEUE_UPLOADED_PHOTO,
    EPHOTO_CMD_FINALIZE_UPLOAD_BATCH,
} ephoto_command_type_t;

typedef struct {
    ephoto_command_type_t type;
    int32_t value_i32;
    ephoto_notification_level_t level;
    char ssid[EPHOTO_MAX_SSID_LEN];
    char password[EPHOTO_MAX_PASSWORD_LEN];
    char text[EPHOTO_MAX_NOTIFICATION_LEN];
    char path[EPHOTO_MAX_PHOTO_PATH_LEN];
} ephoto_command_t;

typedef struct {
    char ssid[EPHOTO_MAX_SSID_LEN];
    char password[EPHOTO_MAX_PASSWORD_LEN];
} ephoto_wifi_profile_t;

typedef struct {
    char ssid[EPHOTO_MAX_SSID_LEN];
    int8_t rssi;
    uint8_t auth_mode;
} ephoto_wifi_scan_result_t;

typedef struct {
    ephoto_playback_mode_t playback_mode;
    ephoto_slideshow_interval_t slideshow_interval;
    ephoto_brightness_t brightness;
    ephoto_brightness_t manual_brightness;
    ephoto_brightness_mode_t brightness_mode;
    ephoto_clock_format_t clock_format;
    ephoto_clock_position_t clock_position;
    ephoto_clock_color_t clock_color;
    ephoto_ota_interval_t ota_interval;
    ephoto_fit_mode_t fit_mode;
    ephoto_orientation_filter_t orientation_filter;
    bool clock_visible;
    bool screen_on;
    bool screen_schedule_enabled;
    bool auto_rotation_enabled;
    bool auto_time_sync;
    uint16_t screen_on_minute;
    uint16_t screen_off_minute;
    uint16_t rotation_deg;
    uint16_t manual_rotation_deg;
    char timezone[EPHOTO_MAX_TIMEZONE_LEN];
    char ota_channel[EPHOTO_MAX_OTA_CHANNEL_LEN];
} ephoto_settings_t;

typedef struct {
    bool present;
    bool mounted;
    uint64_t capacity_bytes;
    uint64_t used_bytes;
    int photo_count;
    esp_err_t last_error;
} ephoto_storage_status_t;

typedef struct {
    char name[EPHOTO_MAX_PHOTO_NAME_LEN];
    char path[EPHOTO_MAX_PHOTO_PATH_LEN];
    size_t size_bytes;
    time_t mtime;
    uint16_t raw_width;
    uint16_t raw_height;
    uint16_t width;
    uint16_t height;
    uint16_t exif_orientation;
    uint16_t effective_rotation_deg;
    uint16_t manual_rotation_deg;
} ephoto_photo_t;

typedef struct {
    ephoto_notification_level_t level;
    char text[EPHOTO_MAX_NOTIFICATION_LEN];
    char screen_text[EPHOTO_MAX_NOTIFICATION_LEN];
    int64_t updated_at_ms;
} ephoto_notification_t;

typedef struct {
    bool visible;
    bool editing;
    uint8_t depth;
    uint8_t selected_index;
    uint8_t item_count;
    uint8_t option_count;
    uint8_t edit_choice_count;
    uint8_t edit_choice_index;
    char title[EPHOTO_MENU_LABEL_LEN];
    char title_key[EPHOTO_MENU_LABEL_LEN];
    char selected_label[EPHOTO_MENU_LABEL_LEN];
    char selected_key[EPHOTO_MENU_LABEL_LEN];
    char selected_value[EPHOTO_MENU_LABEL_LEN];
    char selected_value_key[EPHOTO_MENU_LABEL_LEN];
    char items[EPHOTO_MENU_MAX_ITEMS][EPHOTO_MENU_LABEL_LEN];
    char item_keys[EPHOTO_MENU_MAX_ITEMS][EPHOTO_MENU_LABEL_LEN];
    char item_values[EPHOTO_MENU_MAX_ITEMS][EPHOTO_MENU_LABEL_LEN];
    char edit_choices[EPHOTO_MENU_MAX_ITEMS][EPHOTO_MENU_LABEL_LEN];
} ephoto_menu_state_t;

typedef struct {
    int32_t photo_index;
    uint16_t width;
    uint16_t height;
    bool is_current;
    bool thumb_ready;
    char badge[24];
    char meta[32];
} ephoto_album_slot_t;

typedef struct {
    bool visible;
    bool action_popup_visible;
    bool action_confirm_pending;
    uint8_t slot_count;
    uint8_t selected_slot;
    uint8_t selected_action;
    uint8_t action_count;
    uint32_t total_count;
    uint32_t selected_position;
    uint32_t page_start_position;
    char title[EPHOTO_MENU_LABEL_LEN];
    char summary[64];
    char hint[96];
    char action_title[32];
    char actions[EPHOTO_ALBUM_MAX_ACTIONS][24];
    ephoto_album_slot_t slots[EPHOTO_ALBUM_MAX_SLOTS];
} ephoto_album_state_t;

typedef struct {
    bool available;
    bool queried;
    bool needs_upgrade;
    bool slave_ota_supported;
    int32_t version_relation;
    int64_t last_query_ms;
    esp_err_t last_error;
    uint32_t host_major;
    uint32_t host_minor;
    uint32_t host_patch;
    uint32_t slave_major;
    uint32_t slave_minor;
    uint32_t slave_patch;
} ephoto_hosted_status_t;

typedef struct {
    bool configured;
    bool checking;
    bool update_available;
    bool updating;
    bool reboot_required;
    bool last_check_ok;
    bool last_manual_trigger;
    uint8_t progress_percent;
    ephoto_ota_stage_t stage;
    esp_err_t last_error;
    int64_t last_check_ms;
    int64_t last_update_ms;
    char provider[EPHOTO_MAX_OTA_PROVIDER_LEN];
    char device_type[EPHOTO_MAX_OTA_DEVICE_TYPE_LEN];
    char channel[EPHOTO_MAX_OTA_CHANNEL_LEN];
    char current_version[EPHOTO_MAX_OTA_VERSION_LEN];
    char available_version[EPHOTO_MAX_OTA_VERSION_LEN];
    char available_artifact[EPHOTO_MAX_OTA_ARTIFACT_LEN];
    char deployment_id[EPHOTO_MAX_OTA_DEPLOYMENT_LEN];
    char message[EPHOTO_MAX_OTA_MESSAGE_LEN];
    char release_notes[EPHOTO_MAX_OTA_NOTES_LEN];
} ephoto_ota_status_t;

typedef struct {
    bool custom_available;
    bool custom_selected;
    bool update_in_progress;
    ephoto_boot_image_source_t active_source;
    char active_name[EPHOTO_MAX_BOOT_IMAGE_NAME_LEN];
    int64_t updated_at_ms;
    esp_err_t last_error;
} ephoto_boot_image_status_t;

typedef struct {
    gpio_num_t gpio;
    bool active_level;
    bool pull_up;
} ephoto_button_pin_t;

typedef struct {
    int sdmmc_slot;
    int sdmmc_width;
    bool sdmmc_internal_pullups;
    int sd_pwr_ldo_chan;
    gpio_num_t sd_det;
    bool sd_det_active_low;
    bool sd_det_pull_up;
    gpio_num_t sd_clk;
    gpio_num_t sd_cmd;
    gpio_num_t sd_d0;
    gpio_num_t sd_d1;
    gpio_num_t sd_d2;
    gpio_num_t sd_d3;
    gpio_num_t display_reset;
    gpio_num_t display_power;
    gpio_num_t backlight;
    gpio_num_t ambient_light_gpio;
    gpio_num_t rotation_switch_gpio;
    bool rotation_switch_active_low;
    bool rotation_switch_pull_up;
    bool backlight_pwm;
    int dsi_bus_id;
    uint8_t dsi_num_data_lanes;
    uint16_t dsi_lane_bit_rate_mbps;
    int dsi_phy_ldo_chan;
    int dsi_phy_ldo_voltage_mv;
    uint16_t lcd_h_res;
    uint16_t lcd_v_res;
    uint16_t default_rotation_deg;
    ephoto_brightness_t default_brightness;
    char mount_path[EPHOTO_MAX_MOUNT_PATH_LEN];
    char photo_dir[EPHOTO_MAX_PHOTO_DIR_LEN];
    ephoto_button_pin_t buttons[EPHOTO_BUTTON_COUNT];
} board_profile_t;

typedef struct {
    bool menu_visible;
    bool interaction_locked;
    bool cache_build_active;
    uint8_t menu_index;
    int64_t menu_last_input_ms;
    int32_t current_photo_index;
    uint32_t state_version;
    uint32_t cache_build_total;
    uint32_t cache_build_done;
    uint32_t cache_build_failed;
    uint32_t cache_build_photo_total;
    uint32_t cache_build_photo_index;
    uint16_t ambient_light_raw_mv;
    uint8_t ambient_light_percent;
    ephoto_network_mode_t network_mode;
    char current_ssid[EPHOTO_MAX_SSID_LEN];
    char cache_build_photo_name[EPHOTO_MAX_PHOTO_NAME_LEN];
    char current_photo_name[EPHOTO_MAX_PHOTO_NAME_LEN];
    char current_photo_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    ephoto_settings_t settings;
    ephoto_storage_status_t storage;
    ephoto_notification_t notification;
    ephoto_menu_state_t menu;
    ephoto_album_state_t album;
    ephoto_boot_image_status_t boot_image;
    ephoto_ota_status_t ota;
    ephoto_hosted_status_t hosted;
} ephoto_app_state_t;

const char *ephoto_playback_mode_to_string(ephoto_playback_mode_t mode);
const char *ephoto_interval_to_string(ephoto_slideshow_interval_t interval);
const char *ephoto_fit_mode_to_string(ephoto_fit_mode_t mode);
const char *ephoto_orientation_filter_to_string(ephoto_orientation_filter_t filter);
const char *ephoto_brightness_mode_to_string(ephoto_brightness_mode_t mode);
const char *ephoto_clock_format_to_string(ephoto_clock_format_t format);
const char *ephoto_clock_position_to_string(ephoto_clock_position_t position);
const char *ephoto_clock_color_to_string(ephoto_clock_color_t color);
const char *ephoto_ota_interval_to_string(ephoto_ota_interval_t interval);
const char *ephoto_ota_stage_to_string(ephoto_ota_stage_t stage);
const char *ephoto_network_mode_to_string(ephoto_network_mode_t mode);
const char *ephoto_notification_level_to_string(ephoto_notification_level_t level);
uint64_t ephoto_interval_to_ms(ephoto_slideshow_interval_t interval);
uint64_t ephoto_ota_interval_to_ms(ephoto_ota_interval_t interval);

#ifdef __cplusplus
}
#endif
