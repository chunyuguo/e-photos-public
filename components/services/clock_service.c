#include "clock_service.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"

static const char *TAG = "clock_service";
static const char *DEFAULT_TZ = "CST-8";
static const int64_t EPHOTO_CLOCK_RESYNC_INTERVAL_MS = 6LL * 60LL * 60LL * 1000LL;
static const int64_t EPHOTO_CLOCK_RETRY_INTERVAL_MS = 10LL * 1000LL;
static const time_t EPHOTO_CLOCK_VALID_UNIX_TS = 1704067200;  // 2024-01-01 00:00:00 UTC

static bool s_auto_sync_enabled;
static bool s_sntp_initialized;
static bool s_sync_in_progress;
static int64_t s_last_sync_ms;
static int64_t s_last_attempt_ms;
static char s_last_sync_time[32];
static char s_last_sync_source[16];
static char s_timezone[32];
static bool s_last_network_ready;

static bool clock_time_is_valid(void)
{
    return time(NULL) >= EPHOTO_CLOCK_VALID_UNIX_TS;
}

static void apply_timezone(const char *timezone)
{
    const char *tz = (timezone && timezone[0]) ? timezone : DEFAULT_TZ;
    setenv("TZ", tz, 1);
    tzset();
    strlcpy(s_timezone, tz, sizeof(s_timezone));
}

static void update_last_sync_record(const char *source)
{
    s_last_sync_ms = esp_timer_get_time() / 1000;
    s_sync_in_progress = false;
    strlcpy(s_last_sync_source, source ? source : "sync", sizeof(s_last_sync_source));
    clock_service_get_time_string(s_last_sync_time, sizeof(s_last_sync_time));
}

static void sntp_sync_time_cb(struct timeval *tv)
{
    (void)tv;
    update_last_sync_record("NTP");
    ESP_LOGI(TAG, "time synchronized via SNTP: %s", s_last_sync_time);
}

static void ensure_sntp_started(void)
{
    if (s_sntp_initialized) {
        esp_sntp_restart();
        return;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb);
    esp_sntp_init();
    s_sntp_initialized = true;
}

void clock_service_init(const char *timezone, bool auto_sync_enabled)
{
    apply_timezone(timezone);
    s_auto_sync_enabled = auto_sync_enabled;
    s_sync_in_progress = false;
    s_last_sync_ms = 0;
    s_last_attempt_ms = 0;
    s_last_sync_time[0] = '\0';
    s_last_sync_source[0] = '\0';
    s_last_network_ready = false;
}

void clock_service_set_timezone(const char *timezone)
{
    apply_timezone(timezone);
}

void clock_service_set_auto_sync_enabled(bool enabled)
{
    s_auto_sync_enabled = enabled;
    if (!enabled) {
        s_sync_in_progress = false;
    }
}

esp_err_t clock_service_sync_now(void)
{
    ensure_sntp_started();
    s_last_attempt_ms = esp_timer_get_time() / 1000;
    s_sync_in_progress = true;
    ESP_LOGI(TAG, "starting SNTP sync");
    esp_sntp_restart();
    return ESP_OK;
}

static bool parse_manual_time_string(const char *local_datetime, struct tm *out_tm)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int parsed = 0;

    if (!local_datetime || !out_tm) {
        return false;
    }

    parsed = sscanf(local_datetime, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    if (parsed < 5) {
        parsed = sscanf(local_datetime, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    }
    if (parsed < 5) {
        return false;
    }
    if (parsed == 5) {
        second = 0;
    }

    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = year - 1900;
    out_tm->tm_mon = month - 1;
    out_tm->tm_mday = day;
    out_tm->tm_hour = hour;
    out_tm->tm_min = minute;
    out_tm->tm_sec = second;
    out_tm->tm_isdst = -1;
    return true;
}

esp_err_t clock_service_set_manual_time_string(const char *local_datetime)
{
    struct tm tm_value;
    time_t epoch;
    struct timeval tv = {0};

    if (!parse_manual_time_string(local_datetime, &tm_value)) {
        return ESP_ERR_INVALID_ARG;
    }

    epoch = mktime(&tm_value);
    if (epoch < EPHOTO_CLOCK_VALID_UNIX_TS) {
        return ESP_ERR_INVALID_ARG;
    }

    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    update_last_sync_record("MANUAL");
    ESP_LOGI(TAG, "manual time set: %s", s_last_sync_time);
    return ESP_OK;
}

void clock_service_poll(bool network_ready)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool valid = clock_time_is_valid();

    if (!s_auto_sync_enabled || !network_ready) {
        s_last_network_ready = network_ready;
        return;
    }

    bool network_just_ready = !s_last_network_ready;
    s_last_network_ready = true;
    if (network_just_ready && !s_sync_in_progress) {
        clock_service_sync_now();
        return;
    }

    if (s_sync_in_progress && (now_ms - s_last_attempt_ms) < EPHOTO_CLOCK_RETRY_INTERVAL_MS) {
        return;
    }

    if ((!valid && (now_ms - s_last_attempt_ms) >= EPHOTO_CLOCK_RETRY_INTERVAL_MS) ||
        (valid && (s_last_sync_ms == 0 || (now_ms - s_last_sync_ms) >= EPHOTO_CLOCK_RESYNC_INTERVAL_MS))) {
        clock_service_sync_now();
    }
}

void clock_service_get_status(clock_service_status_t *status)
{
    if (!status) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->auto_sync_enabled = s_auto_sync_enabled;
    status->sync_in_progress = s_sync_in_progress;
    status->time_valid = clock_time_is_valid();
    status->last_sync_ms = s_last_sync_ms;
    strlcpy(status->last_sync_time, s_last_sync_time, sizeof(status->last_sync_time));
    strlcpy(status->last_sync_source, s_last_sync_source, sizeof(status->last_sync_source));
    strlcpy(status->timezone, s_timezone[0] ? s_timezone : DEFAULT_TZ, sizeof(status->timezone));
}

void clock_service_get_time_string(char *buffer, size_t buffer_len)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buffer, buffer_len, "%Y-%m-%d %H:%M:%S", &tm_now);
}

bool clock_service_get_local_tm(struct tm *out_tm)
{
    if (!out_tm) {
        return false;
    }

    time_t now = time(NULL);
    return localtime_r(&now, out_tm) != NULL;
}

int64_t clock_service_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

int64_t clock_service_wall_time_ms(void)
{
    struct timeval tv = {0};
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    if (tv.tv_sec <= 0) {
        return 0;
    }
    return ((int64_t)tv.tv_sec * 1000LL) + ((int64_t)tv.tv_usec / 1000LL);
}
