#include "ephoto_types.h"

const char *ephoto_playback_mode_to_string(ephoto_playback_mode_t mode)
{
    switch (mode) {
    case EPHOTO_PLAYBACK_TIME_ASC:
        return "time_asc";
    case EPHOTO_PLAYBACK_TIME_DESC:
        return "time_desc";
    case EPHOTO_PLAYBACK_RANDOM:
        return "random";
    default:
        return "unknown";
    }
}

const char *ephoto_interval_to_string(ephoto_slideshow_interval_t interval)
{
    switch (interval) {
    case EPHOTO_INTERVAL_OFF:
        return "off";
    case EPHOTO_INTERVAL_10S:
        return "10s";
    case EPHOTO_INTERVAL_1M:
        return "1m";
    case EPHOTO_INTERVAL_30M:
        return "30m";
    case EPHOTO_INTERVAL_60M:
        return "60m";
    case EPHOTO_INTERVAL_6H:
        return "6h";
    case EPHOTO_INTERVAL_12H:
        return "12h";
    case EPHOTO_INTERVAL_24H:
        return "24h";
    case EPHOTO_INTERVAL_1W:
        return "1w";
    case EPHOTO_INTERVAL_1MO:
        return "1mo";
    default:
        return "unknown";
    }
}

const char *ephoto_network_mode_to_string(ephoto_network_mode_t mode)
{
    switch (mode) {
    case EPHOTO_NETWORK_UNAVAILABLE:
        return "unavailable";
    case EPHOTO_NETWORK_DISCONNECTED:
        return "disconnected";
    case EPHOTO_NETWORK_AP:
        return "ap";
    case EPHOTO_NETWORK_STA:
        return "sta";
    default:
        return "unknown";
    }
}

const char *ephoto_fit_mode_to_string(ephoto_fit_mode_t mode)
{
    switch (mode) {
    case EPHOTO_FIT_CONTAIN:
        return "contain";
    case EPHOTO_FIT_COVER:
        return "cover";
    default:
        return "unknown";
    }
}

const char *ephoto_orientation_filter_to_string(ephoto_orientation_filter_t filter)
{
    switch (filter) {
    case EPHOTO_ORIENTATION_ALL:
        return "all";
    case EPHOTO_ORIENTATION_LANDSCAPE:
        return "landscape";
    case EPHOTO_ORIENTATION_PORTRAIT:
        return "portrait";
    default:
        return "unknown";
    }
}

const char *ephoto_brightness_mode_to_string(ephoto_brightness_mode_t mode)
{
    switch (mode) {
    case EPHOTO_BRIGHTNESS_MODE_AUTO:
        return "auto";
    case EPHOTO_BRIGHTNESS_MODE_MANUAL:
    default:
        return "manual";
    }
}

const char *ephoto_clock_format_to_string(ephoto_clock_format_t format)
{
    switch (format) {
    case EPHOTO_CLOCK_FORMAT_12H:
        return "12h";
    case EPHOTO_CLOCK_FORMAT_24H:
        return "24h";
    default:
        return "unknown";
    }
}

const char *ephoto_clock_position_to_string(ephoto_clock_position_t position)
{
    switch (position) {
    case EPHOTO_CLOCK_POSITION_TOP_LEFT:
        return "top_left";
    case EPHOTO_CLOCK_POSITION_BOTTOM_LEFT:
        return "bottom_left";
    case EPHOTO_CLOCK_POSITION_BOTTOM_RIGHT:
        return "bottom_right";
    case EPHOTO_CLOCK_POSITION_TOP_RIGHT:
    default:
        return "top_right";
    }
}

const char *ephoto_clock_color_to_string(ephoto_clock_color_t color)
{
    switch (color) {
    case EPHOTO_CLOCK_COLOR_DARK_GRAY:
        return "dark_gray";
    case EPHOTO_CLOCK_COLOR_WHITE:
    default:
        return "white";
    }
}

const char *ephoto_ota_interval_to_string(ephoto_ota_interval_t interval)
{
    switch (interval) {
    case EPHOTO_OTA_INTERVAL_6H:
    case EPHOTO_OTA_INTERVAL_24H:
        return "24h";
    case EPHOTO_OTA_INTERVAL_1W:
        return "1w";
    case EPHOTO_OTA_INTERVAL_OFF:
    default:
        return "off";
    }
}

const char *ephoto_ota_stage_to_string(ephoto_ota_stage_t stage)
{
    switch (stage) {
    case EPHOTO_OTA_STAGE_CHECKING:
        return "checking";
    case EPHOTO_OTA_STAGE_UP_TO_DATE:
        return "up_to_date";
    case EPHOTO_OTA_STAGE_UPDATE_AVAILABLE:
        return "available";
    case EPHOTO_OTA_STAGE_DOWNLOADING:
        return "downloading";
    case EPHOTO_OTA_STAGE_APPLYING:
        return "applying";
    case EPHOTO_OTA_STAGE_RESTARTING:
        return "restarting";
    case EPHOTO_OTA_STAGE_SUCCESS:
        return "success";
    case EPHOTO_OTA_STAGE_ERROR:
        return "error";
    case EPHOTO_OTA_STAGE_IDLE:
    default:
        return "idle";
    }
}

const char *ephoto_notification_level_to_string(ephoto_notification_level_t level)
{
    switch (level) {
    case EPHOTO_NOTIFICATION_INFO:
        return "info";
    case EPHOTO_NOTIFICATION_SUCCESS:
        return "success";
    case EPHOTO_NOTIFICATION_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

uint64_t ephoto_interval_to_ms(ephoto_slideshow_interval_t interval)
{
    switch (interval) {
    case EPHOTO_INTERVAL_OFF:
        return 0;
    case EPHOTO_INTERVAL_10S:
        return 10ULL * 1000ULL;
    case EPHOTO_INTERVAL_1M:
        return 60ULL * 1000ULL;
    case EPHOTO_INTERVAL_30M:
        return 30ULL * 60ULL * 1000ULL;
    case EPHOTO_INTERVAL_60M:
        return 60ULL * 60ULL * 1000ULL;
    case EPHOTO_INTERVAL_6H:
        return 6ULL * 60ULL * 60ULL * 1000ULL;
    case EPHOTO_INTERVAL_12H:
        return 12ULL * 60ULL * 60ULL * 1000ULL;
    case EPHOTO_INTERVAL_24H:
        return 24ULL * 60ULL * 60ULL * 1000ULL;
    case EPHOTO_INTERVAL_1W:
        return 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
    case EPHOTO_INTERVAL_1MO:
        return 30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
    default:
        return 0;
    }
}

uint64_t ephoto_ota_interval_to_ms(ephoto_ota_interval_t interval)
{
    switch (interval) {
    case EPHOTO_OTA_INTERVAL_6H:
    case EPHOTO_OTA_INTERVAL_24H:
        return 24ULL * 60ULL * 60ULL * 1000ULL;
    case EPHOTO_OTA_INTERVAL_1W:
        return 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
    case EPHOTO_OTA_INTERVAL_OFF:
    default:
        return 0;
    }
}
