#include "web_api.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cJSON.h"
#include "clock_service.h"
#include "boot_image_service.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "factory_reset_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "gallery_service.h"
#include "mbedtls/base64.h"
#include "notification_service.h"
#include "ota_service.h"
#include "storage_service.h"
#include "wifi_admin.h"

static const char *TAG = "web_api";
static httpd_handle_t s_server;
static web_state_snapshot_fn_t s_snapshot_fn;
static input_command_handler_t s_command_handler;

typedef enum {
    EPHOTO_WEB_MODE_CONTROL = 0,
    EPHOTO_WEB_MODE_ALBUM,
    EPHOTO_WEB_MODE_UPLOAD,
} ephoto_web_mode_t;

static ephoto_web_mode_t s_web_mode = EPHOTO_WEB_MODE_CONTROL;
static int64_t s_web_mode_updated_at_ms;
static SemaphoreHandle_t s_upload_finalize_mutex;
static SemaphoreHandle_t s_upload_write_mutex;
static esp_timer_handle_t s_upload_finalize_timer;
static bool s_upload_session_active;
static bool s_deferred_upload_finalize_pending;
static bool s_upload_recovered_by_watchdog;
static uint32_t s_deferred_upload_generation;
static uint32_t s_upload_completed_count;
static int64_t s_upload_session_started_at_ms;
static char s_deferred_upload_latest_path[EPHOTO_MAX_PHOTO_PATH_LEN];
static char s_upload_last_recovery_message[EPHOTO_MAX_NOTIFICATION_LEN];

#define EPHOTO_UPLOAD_IO_CHUNK  (64 * 1024)
#define EPHOTO_HTTP_STREAM_CHUNK (48 * 1024)
#define EPHOTO_HTTP_INLINE_LIMIT (256 * 1024)
#define EPHOTO_MULTIPART_BOUNDARY_MAX_LEN 256
#define EPHOTO_MULTIPART_HEADER_MAX_LEN   2048
#define EPHOTO_DEFERRED_UPLOAD_FINALIZE_TIMEOUT_US (60LL * 1000 * 1000)
#define EPHOTO_PHOTO_PAGE_DEFAULT_LIMIT 66U
#define EPHOTO_PHOTO_PAGE_MAX_LIMIT     66U
#define EPHOTO_PHOTO_PAGE_MAX_OFFSET    1000000U
#define EPHOTO_HTTP_MAX_OPEN_SOCKETS     10U

typedef struct {
    bool active;
    bool recovery_pending;
    bool recovered_by_watchdog;
    int64_t session_started_at_ms;
    uint32_t completed_count;
    char last_recovery_message[EPHOTO_MAX_NOTIFICATION_LEN];
} upload_status_snapshot_t;

static uint8_t *allocate_upload_dma_buffer(size_t *out_size)
{
    static const size_t sizes[] = {32 * 1024, 16 * 1024, 8 * 1024};

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        uint8_t *buffer = heap_caps_malloc(sizes[i], MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buffer) {
            *out_size = sizes[i];
            return buffer;
        }
    }
    return NULL;
}

static bool write_upload_chunk(FILE *file,
                               uint8_t *dma_buffer,
                               size_t dma_buffer_size,
                               size_t *pending_bytes,
                               const void *data,
                               size_t bytes)
{
    const uint8_t *source = data;

    while (bytes > 0) {
        size_t room = dma_buffer_size - *pending_bytes;
        size_t chunk = bytes < room ? bytes : room;
        memcpy(dma_buffer + *pending_bytes, source, chunk);
        *pending_bytes += chunk;
        source += chunk;
        bytes -= chunk;

        if (*pending_bytes == dma_buffer_size) {
            if (fwrite(dma_buffer, 1, *pending_bytes, file) != *pending_bytes) {
                return false;
            }
            *pending_bytes = 0;
        }
    }
    return true;
}

static bool flush_upload_chunk(FILE *file, uint8_t *dma_buffer, size_t *pending_bytes)
{
    if (*pending_bytes > 0) {
        if (fwrite(dma_buffer, 1, *pending_bytes, file) != *pending_bytes) {
            return false;
        }
        *pending_bytes = 0;
    }
    return true;
}

static const char *firmware_version_string(void)
{
#ifdef EPHOTO_PROJECT_VER
    return EPHOTO_PROJECT_VER;
#else
    return "unknown";
#endif
}

static const char *web_mode_to_string(ephoto_web_mode_t mode)
{
    switch (mode) {
    case EPHOTO_WEB_MODE_ALBUM:
        return "album";
    case EPHOTO_WEB_MODE_UPLOAD:
        return "upload";
    case EPHOTO_WEB_MODE_CONTROL:
    default:
        return "control";
    }
}

static ephoto_web_mode_t parse_web_mode_string(const char *value)
{
    if (!value || !value[0]) {
        return EPHOTO_WEB_MODE_CONTROL;
    }
    if (strcmp(value, "album") == 0) {
        return EPHOTO_WEB_MODE_ALBUM;
    }
    if (strcmp(value, "upload") == 0) {
        return EPHOTO_WEB_MODE_UPLOAD;
    }
    return EPHOTO_WEB_MODE_CONTROL;
}

static void set_web_mode(ephoto_web_mode_t mode)
{
    s_web_mode = mode;
    s_web_mode_updated_at_ms = clock_service_now_ms();
}

static bool web_mode_blocks_album_io(void)
{
    return s_web_mode == EPHOTO_WEB_MODE_UPLOAD;
}

static bool web_mode_blocks_heavy_actions(const char *action)
{
    if (s_web_mode != EPHOTO_WEB_MODE_UPLOAD || !action) {
        return false;
    }
    return strcmp(action, "prev") == 0 ||
           strcmp(action, "next") == 0 ||
           strcmp(action, "rotate") == 0 ||
           strcmp(action, "menu") == 0 ||
           strcmp(action, "confirm") == 0 ||
           strcmp(action, "cache-purge") == 0 ||
           strcmp(action, "build-cache") == 0 ||
           strcmp(action, "reprocess-current") == 0 ||
           strcmp(action, "rescan-media") == 0;
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");

static esp_err_t read_json_body(httpd_req_t *req, cJSON **out_json);
static esp_err_t send_screen_cached_photo_jpeg(httpd_req_t *req,
                                               const char *source_path,
                                               ephoto_fit_mode_t fit_mode,
                                               uint16_t rotation_deg);
static esp_err_t send_web_thumbnail_jpeg(httpd_req_t *req,
                                         const char *source_path,
                                         ephoto_fit_mode_t fit_mode);
static const char *web_mode_to_string(ephoto_web_mode_t mode);
static ephoto_web_mode_t parse_web_mode_string(const char *value);
static esp_err_t enqueue_upload_finalize_command(const char *latest_path, const char *message);

static void upload_finalize_watchdog_arm_locked(void)
{
    if (!s_upload_finalize_timer) {
        return;
    }
    if (esp_timer_is_active(s_upload_finalize_timer)) {
        (void)esp_timer_stop(s_upload_finalize_timer);
    }
    (void)esp_timer_start_once(s_upload_finalize_timer, EPHOTO_DEFERRED_UPLOAD_FINALIZE_TIMEOUT_US);
}

static void upload_finalize_watchdog_touch(void)
{
    if (!s_upload_finalize_mutex) {
        return;
    }
    xSemaphoreTake(s_upload_finalize_mutex, portMAX_DELAY);
    if (!s_upload_session_active) {
        s_upload_session_started_at_ms = clock_service_now_ms();
        s_upload_completed_count = 0;
        s_upload_recovered_by_watchdog = false;
        s_upload_last_recovery_message[0] = '\0';
    }
    s_upload_session_active = true;
    upload_finalize_watchdog_arm_locked();
    xSemaphoreGive(s_upload_finalize_mutex);
}

static void upload_finalize_watchdog_record(const char *latest_path)
{
    if (!s_upload_finalize_mutex || !latest_path || !latest_path[0]) {
        return;
    }
    xSemaphoreTake(s_upload_finalize_mutex, portMAX_DELAY);
    s_upload_session_active = true;
    s_deferred_upload_finalize_pending = true;
    s_deferred_upload_generation += 1;
    strlcpy(s_deferred_upload_latest_path, latest_path, sizeof(s_deferred_upload_latest_path));
    upload_finalize_watchdog_arm_locked();
    xSemaphoreGive(s_upload_finalize_mutex);
}

static void upload_session_note_completed_file(void)
{
    if (!s_upload_finalize_mutex) {
        return;
    }
    xSemaphoreTake(s_upload_finalize_mutex, portMAX_DELAY);
    s_upload_completed_count += 1;
    xSemaphoreGive(s_upload_finalize_mutex);
}

static void upload_finalize_watchdog_clear(void)
{
    if (!s_upload_finalize_mutex) {
        return;
    }
    xSemaphoreTake(s_upload_finalize_mutex, portMAX_DELAY);
    s_upload_session_active = false;
    s_deferred_upload_finalize_pending = false;
    s_upload_session_started_at_ms = 0;
    s_upload_recovered_by_watchdog = false;
    s_deferred_upload_latest_path[0] = '\0';
    s_upload_last_recovery_message[0] = '\0';
    if (s_upload_finalize_timer && esp_timer_is_active(s_upload_finalize_timer)) {
        (void)esp_timer_stop(s_upload_finalize_timer);
    }
    xSemaphoreGive(s_upload_finalize_mutex);
}

static void upload_finalize_watchdog_callback(void *arg)
{
    (void)arg;
    char latest_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    uint32_t generation = 0;

    if (!s_upload_finalize_mutex) {
        return;
    }
    xSemaphoreTake(s_upload_finalize_mutex, portMAX_DELAY);
    if (!s_upload_session_active) {
        xSemaphoreGive(s_upload_finalize_mutex);
        return;
    }
    if (!s_deferred_upload_finalize_pending || !s_deferred_upload_latest_path[0]) {
        s_upload_session_active = false;
        s_upload_session_started_at_ms = 0;
        s_upload_recovered_by_watchdog = false;
        if (s_upload_completed_count == 0) {
            strlcpy(s_upload_last_recovery_message,
                    "upload session timed out before a file was committed",
                    sizeof(s_upload_last_recovery_message));
        } else {
            snprintf(s_upload_last_recovery_message,
                     sizeof(s_upload_last_recovery_message),
                     "upload session ended after %" PRIu32 " committed files without a recovery queue",
                     s_upload_completed_count);
        }
        xSemaphoreGive(s_upload_finalize_mutex);
        set_web_mode(EPHOTO_WEB_MODE_CONTROL);
        ESP_LOGW(TAG, "upload session timed out before a file was committed");
        return;
    }
    generation = s_deferred_upload_generation;
    strlcpy(latest_path, s_deferred_upload_latest_path, sizeof(latest_path));
    xSemaphoreGive(s_upload_finalize_mutex);

    esp_err_t err = enqueue_upload_finalize_command(latest_path, "上传连接已结束，正在整理已保存照片");

    xSemaphoreTake(s_upload_finalize_mutex, portMAX_DELAY);
    if (s_deferred_upload_finalize_pending && generation == s_deferred_upload_generation) {
        if (err == ESP_OK) {
            s_upload_session_active = false;
            s_deferred_upload_finalize_pending = false;
            s_upload_session_started_at_ms = 0;
            s_upload_recovered_by_watchdog = true;
            s_deferred_upload_latest_path[0] = '\0';
            snprintf(s_upload_last_recovery_message,
                     sizeof(s_upload_last_recovery_message),
                     "upload session timed out; finalized %" PRIu32 " files",
                     s_upload_completed_count);
        } else {
            upload_finalize_watchdog_arm_locked();
        }
    }
    xSemaphoreGive(s_upload_finalize_mutex);

    if (err == ESP_OK) {
        set_web_mode(EPHOTO_WEB_MODE_CONTROL);
        ESP_LOGW(TAG, "upload batch finalize timed out; queued recovery for %s", latest_path);
    } else {
        ESP_LOGW(TAG, "upload batch recovery queue failed: %s", esp_err_to_name(err));
    }
}

static void upload_status_snapshot(upload_status_snapshot_t *out_status)
{
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (!s_upload_finalize_mutex) {
        return;
    }
    xSemaphoreTake(s_upload_finalize_mutex, portMAX_DELAY);
    out_status->active = s_upload_session_active;
    out_status->recovery_pending = s_deferred_upload_finalize_pending;
    out_status->recovered_by_watchdog = s_upload_recovered_by_watchdog;
    out_status->session_started_at_ms = s_upload_session_started_at_ms;
    out_status->completed_count = s_upload_completed_count;
    strlcpy(out_status->last_recovery_message,
            s_upload_last_recovery_message,
            sizeof(out_status->last_recovery_message));
    xSemaphoreGive(s_upload_finalize_mutex);
}
static void set_web_mode(ephoto_web_mode_t mode);
static bool web_mode_blocks_album_io(void);
static esp_err_t handle_set_boot_image_from_photo(httpd_req_t *req);
static esp_err_t handle_set_custom_boot_image(httpd_req_t *req);
static esp_err_t handle_restore_default_boot_image(httpd_req_t *req);
static esp_err_t handle_factory_reset(httpd_req_t *req);

static esp_err_t send_json_response(httpd_req_t *req, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    if (!payload) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    cJSON_Delete(json);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *message, int code)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", ok);
    cJSON_AddStringToObject(json, "message", message ? message : "");
    cJSON_AddNumberToObject(json, "code", code);
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return send_json_response(req, json);
}

static bool interaction_locked_now(void)
{
    if (!s_snapshot_fn) {
        return false;
    }
    ephoto_app_state_t snapshot = {0};
    s_snapshot_fn(&snapshot);
    return snapshot.interaction_locked;
}

static bool auto_rotation_enabled_now(void)
{
    if (!s_snapshot_fn) {
        return false;
    }
    ephoto_app_state_t snapshot = {0};
    s_snapshot_fn(&snapshot);
    return snapshot.settings.auto_rotation_enabled;
}

static const uint8_t *find_bytes(const uint8_t *haystack,
                                 size_t haystack_len,
                                 const char *needle,
                                 size_t needle_len)
{
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }

    size_t last = haystack_len - needle_len;
    for (size_t i = 0; i <= last; ++i) {
        if (haystack[i] == (uint8_t)needle[0] &&
            memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static bool parse_multipart_boundary(const char *content_type, char *boundary, size_t boundary_len)
{
    if (!content_type || !boundary || boundary_len < 4) {
        return false;
    }

    const char *multipart = strstr(content_type, "multipart/form-data");
    if (!multipart) {
        return false;
    }

    const char *boundary_key = strstr(content_type, "boundary=");
    if (!boundary_key) {
        return false;
    }
    boundary_key += strlen("boundary=");

    if (*boundary_key == '"') {
        boundary_key += 1;
        const char *end_quote = strchr(boundary_key, '"');
        if (!end_quote) {
            return false;
        }
        size_t len = (size_t)(end_quote - boundary_key);
        if (len == 0 || len >= boundary_len) {
            return false;
        }
        memcpy(boundary, boundary_key, len);
        boundary[len] = '\0';
        return true;
    }

    const char *end = boundary_key;
    while (*end && *end != ';' && !isspace((unsigned char)*end)) {
        end += 1;
    }
    size_t len = (size_t)(end - boundary_key);
    if (len == 0 || len >= boundary_len) {
        return false;
    }
    memcpy(boundary, boundary_key, len);
    boundary[len] = '\0';
    return true;
}

static bool has_supported_upload_extension(const char *filename)
{
    const char *ext = filename ? strrchr(filename, '.') : NULL;
    if (!ext) {
        return false;
    }
    return strcasecmp(ext, ".jpg") == 0 ||
           strcasecmp(ext, ".jpeg") == 0 ||
           strcasecmp(ext, ".png") == 0 ||
           strcasecmp(ext, ".bmp") == 0;
}

static bool sniff_supported_upload_payload(const uint8_t *header, size_t bytes_read)
{
    if (!header || bytes_read < 2) {
        return false;
    }
    if (bytes_read >= 4 && header[0] == 0xFF && header[1] == 0xD8) {
        return true;
    }
    if (bytes_read >= 8 && memcmp(header, "\x89PNG\r\n\x1A\n", 8) == 0) {
        return true;
    }
    if (header[0] == 'B' && header[1] == 'M') {
        return true;
    }
    return false;
}

static bool validate_uploaded_temp_file(const char *temp_path,
                                        const char *filename,
                                        char *message,
                                        size_t message_len)
{
    uint8_t header[16] = {0};
    FILE *file = NULL;
    size_t bytes_read = 0;

    if (!temp_path || !filename) {
        if (message && message_len > 0) {
            strlcpy(message, "上传文件无效", message_len);
        }
        return false;
    }

    if (!has_supported_upload_extension(filename)) {
        if (message && message_len > 0) {
            strlcpy(message,
                    "不支持该文件类型。当前仅支持 JPEG / PNG / BMP 图片，请重新选择照片后上传。",
                    message_len);
        }
        return false;
    }

    file = fopen(temp_path, "rb");
    if (!file) {
        if (message && message_len > 0) {
            strlcpy(message, "无法读取上传文件，请重试", message_len);
        }
        return false;
    }

    bytes_read = fread(header, 1, sizeof(header), file);
    fclose(file);

    if (!sniff_supported_upload_payload(header, bytes_read)) {
        if (message && message_len > 0) {
            strlcpy(message,
                    "文件已收到，但内容不是受支持的图片格式。当前仅支持 JPEG / PNG / BMP 图片。",
                    message_len);
        }
        return false;
    }

    return true;
}

static bool photo_matches_orientation_filter_web(const ephoto_photo_t *photo,
                                                 ephoto_orientation_filter_t filter)
{
    if (!photo || filter == EPHOTO_ORIENTATION_ALL) {
        return true;
    }
    if (filter == EPHOTO_ORIENTATION_LANDSCAPE) {
        return photo->width >= photo->height;
    }
    if (filter == EPHOTO_ORIENTATION_PORTRAIT) {
        return photo->height > photo->width;
    }
    return true;
}

static bool photo_supports_orientation_adjustment_web(const ephoto_photo_t *photo)
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

static void build_orientation_block_message(const ephoto_photo_t *photo,
                                            ephoto_orientation_filter_t filter,
                                            char *out,
                                            size_t out_len)
{
    const char *photo_label = (photo && photo->height > photo->width) ? "竖图" : "横图";
    const char *filter_label = filter == EPHOTO_ORIENTATION_LANDSCAPE ? "横图" : "竖图";
    const char *suggest_label = filter == EPHOTO_ORIENTATION_LANDSCAPE ? "仅竖屏图片" : "仅横屏图片";

    if (!out || out_len == 0) {
        return;
    }

    snprintf(out,
             out_len,
             "当前是仅%s模式，这张照片是%s，暂时不能显示。请先改成“全部显示”或“%s”后再试。",
             filter_label,
             photo_label,
             suggest_label);
}

static esp_err_t send_embedded_text(httpd_req_t *req,
                                    const char *content_type,
                                    const uint8_t *start,
                                    const uint8_t *end)
{
    if (!start || !end || end < start) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    return httpd_resp_send(req, (const char *)start, (ssize_t)len);
}

static ephoto_slideshow_interval_t parse_interval(const char *value)
{
    if (!value) {
        return EPHOTO_INTERVAL_10S;
    }
    if (strcmp(value, "off") == 0) return EPHOTO_INTERVAL_OFF;
    if (strcmp(value, "10s") == 0) return EPHOTO_INTERVAL_10S;
    if (strcmp(value, "1m") == 0) return EPHOTO_INTERVAL_1M;
    if (strcmp(value, "30m") == 0) return EPHOTO_INTERVAL_30M;
    if (strcmp(value, "60m") == 0) return EPHOTO_INTERVAL_60M;
    if (strcmp(value, "6h") == 0) return EPHOTO_INTERVAL_6H;
    if (strcmp(value, "12h") == 0) return EPHOTO_INTERVAL_12H;
    if (strcmp(value, "24h") == 0) return EPHOTO_INTERVAL_24H;
    if (strcmp(value, "1w") == 0) return EPHOTO_INTERVAL_1W;
    return EPHOTO_INTERVAL_1MO;
}

static ephoto_playback_mode_t parse_mode(const char *value)
{
    if (!value) {
        return EPHOTO_PLAYBACK_TIME_ASC;
    }
    if (strcmp(value, "time_desc") == 0) return EPHOTO_PLAYBACK_TIME_DESC;
    if (strcmp(value, "random") == 0) return EPHOTO_PLAYBACK_RANDOM;
    return EPHOTO_PLAYBACK_TIME_ASC;
}

static ephoto_fit_mode_t parse_fit_mode(const char *value)
{
    if (value && strcmp(value, "cover") == 0) {
        return EPHOTO_FIT_COVER;
    }
    return EPHOTO_FIT_CONTAIN;
}

static ephoto_brightness_mode_t parse_brightness_mode(const char *value)
{
    if (value && strcmp(value, "auto") == 0) {
        return EPHOTO_BRIGHTNESS_MODE_AUTO;
    }
    return EPHOTO_BRIGHTNESS_MODE_MANUAL;
}

static ephoto_orientation_filter_t parse_orientation_filter(const char *value)
{
    if (!value) {
        return EPHOTO_ORIENTATION_ALL;
    }
    if (strcmp(value, "landscape") == 0) {
        return EPHOTO_ORIENTATION_LANDSCAPE;
    }
    if (strcmp(value, "portrait") == 0) {
        return EPHOTO_ORIENTATION_PORTRAIT;
    }
    return EPHOTO_ORIENTATION_ALL;
}

static ephoto_clock_format_t parse_clock_format(const char *value)
{
    if (!value) {
        return EPHOTO_CLOCK_FORMAT_24H;
    }
    if (strcmp(value, "12h") == 0 || strcmp(value, "analog") == 0) {
        return EPHOTO_CLOCK_FORMAT_12H;
    }
    return EPHOTO_CLOCK_FORMAT_24H;
}

static ephoto_clock_position_t parse_clock_position(const char *value)
{
    if (!value) {
        return EPHOTO_CLOCK_POSITION_TOP_RIGHT;
    }
    if (strcmp(value, "top_left") == 0) {
        return EPHOTO_CLOCK_POSITION_TOP_LEFT;
    }
    if (strcmp(value, "bottom_left") == 0) {
        return EPHOTO_CLOCK_POSITION_BOTTOM_LEFT;
    }
    if (strcmp(value, "bottom_right") == 0) {
        return EPHOTO_CLOCK_POSITION_BOTTOM_RIGHT;
    }
    return EPHOTO_CLOCK_POSITION_TOP_RIGHT;
}

static ephoto_clock_color_t parse_clock_color(const char *value)
{
    if (!value) {
        return EPHOTO_CLOCK_COLOR_WHITE;
    }
    if (strcmp(value, "dark_gray") == 0) {
        return EPHOTO_CLOCK_COLOR_DARK_GRAY;
    }
    return EPHOTO_CLOCK_COLOR_WHITE;
}

static ephoto_ota_interval_t parse_ota_interval(const char *value)
{
    if (!value) {
        return EPHOTO_OTA_INTERVAL_OFF;
    }
    if (strcmp(value, "6h") == 0) {
        return EPHOTO_OTA_INTERVAL_24H;
    }
    if (strcmp(value, "24h") == 0) {
        return EPHOTO_OTA_INTERVAL_24H;
    }
    if (strcmp(value, "1w") == 0) {
        return EPHOTO_OTA_INTERVAL_1W;
    }
    return EPHOTO_OTA_INTERVAL_OFF;
}

static void format_hhmm_string(uint16_t minute_of_day, char *out, size_t out_len)
{
    uint16_t normalized = minute_of_day < 1440U ? minute_of_day : 0U;
    snprintf(out, out_len, "%02u:%02u", normalized / 60U, normalized % 60U);
}

static bool parse_hhmm_string(const char *value, uint16_t *out_minute)
{
    int hour = 0;
    int minute = 0;
    if (!value || strlen(value) < 4 || !out_minute) {
        return false;
    }
    if (sscanf(value, "%d:%d", &hour, &minute) != 2) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    *out_minute = (uint16_t)(hour * 60 + minute);
    return true;
}

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool decode_base64_filename(const char *src, char *dst, size_t dst_size)
{
    if (!src || !src[0] || !dst || dst_size < 2) {
        return false;
    }

    size_t output_len = 0;
    int rc = mbedtls_base64_decode((unsigned char *)dst,
                                   dst_size - 1,
                                   &output_len,
                                   (const unsigned char *)src,
                                   strlen(src));
    if (rc != 0 || output_len == 0 || output_len >= dst_size) {
        return false;
    }
    dst[output_len] = '\0';
    return true;
}

static bool decode_uri_component(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) {
        return false;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; ++i) {
        char ch = src[i];
        if (ch == '%' && src[i + 1] != '\0' && src[i + 2] != '\0') {
            int hi = hex_to_int(src[i + 1]);
            int lo = hex_to_int(src[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            ch = (char)((hi << 4) | lo);
            i += 2;
        } else if (ch == '+') {
            ch = ' ';
        }

        if (out + 1 >= dst_size) {
            return false;
        }
        dst[out++] = ch;
    }

    dst[out] = '\0';
    return true;
}

static bool encode_uri_component(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    if (!src || !dst || dst_size == 0) {
        return false;
    }

    for (size_t i = 0; src[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)src[i];
        bool safe = (ch >= 'A' && ch <= 'Z') ||
                    (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (safe) {
            if (out + 1 >= dst_size) {
                return false;
            }
            dst[out++] = (char)ch;
        } else {
            if (out + 3 >= dst_size) {
                return false;
            }
            dst[out++] = '%';
            dst[out++] = hex[(ch >> 4) & 0x0F];
            dst[out++] = hex[ch & 0x0F];
        }
    }

    dst[out] = '\0';
    return true;
}

static bool photo_path_is_valid_gallery_item(const char *path)
{
    return path && path[0] && gallery_service_find_index_by_path(path) >= 0;
}

static void build_screen_preview_url(const char *path,
                                     ephoto_fit_mode_t fit_mode,
                                     uint16_t rotation_deg,
                                     const ephoto_photo_t *photo,
                                     char *out_url,
                                     size_t out_url_len)
{
    char encoded_path[EPHOTO_MAX_PHOTO_PATH_LEN * 3] = {0};
    if (!path || !out_url || out_url_len == 0 || !encode_uri_component(path, encoded_path, sizeof(encoded_path))) {
        if (out_url && out_url_len > 0) {
            out_url[0] = '\0';
        }
        return;
    }

    snprintf(out_url,
             out_url_len,
             "/api/v1/thumb/%s?asset=screen&fit=%s&r=%u&mt=%" PRIu64 "&er=%u&mr=%u",
             encoded_path,
             fit_mode == EPHOTO_FIT_COVER ? "cover" : "contain",
             rotation_deg == 90 ? 90 : 0,
             photo ? (uint64_t)(photo->mtime > 0 ? photo->mtime : 0) : 0ULL,
             photo ? photo->effective_rotation_deg : 0U,
             photo ? photo->manual_rotation_deg : 0U);
}

static void build_web_thumbnail_url(const char *path,
                                    ephoto_fit_mode_t fit_mode,
                                    const ephoto_photo_t *photo,
                                    char *out_url,
                                    size_t out_url_len)
{
    char encoded_path[EPHOTO_MAX_PHOTO_PATH_LEN * 3] = {0};
    if (!path || !out_url || out_url_len == 0 || !encode_uri_component(path, encoded_path, sizeof(encoded_path))) {
        if (out_url && out_url_len > 0) {
            out_url[0] = '\0';
        }
        return;
    }

    snprintf(out_url,
             out_url_len,
             "/api/v1/thumb/%s?fit=%s&mt=%" PRIu64 "&er=%u&mr=%u",
             encoded_path,
             fit_mode == EPHOTO_FIT_COVER ? "cover" : "contain",
             photo ? (uint64_t)(photo->mtime > 0 ? photo->mtime : 0) : 0ULL,
             photo ? photo->effective_rotation_deg : 0U,
             photo ? photo->manual_rotation_deg : 0U);
}

static esp_err_t send_file_chunked(httpd_req_t *req,
                                   const char *content_type,
                                   const char *path,
                                   size_t offset,
                                   size_t length,
                                   const char *cache_control,
                                   bool allow_inline)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, NULL, 0);
    }

    if (offset > 0 && fseek(fp, (long)offset, SEEK_SET) != 0) {
        fclose(fp);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req,
                       "Cache-Control",
                       cache_control ? cache_control : "no-store, no-cache, must-revalidate, max-age=0");

    if (allow_inline && length > 0 && length <= EPHOTO_HTTP_INLINE_LIMIT) {
        uint8_t *inline_buffer = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!inline_buffer) {
            inline_buffer = malloc(length);
        }
        if (inline_buffer) {
            size_t bytes_read = fread(inline_buffer, 1, length, fp);
            fclose(fp);
            if (bytes_read != length) {
                free(inline_buffer);
                httpd_resp_set_status(req, "500 Internal Server Error");
                return httpd_resp_send(req, NULL, 0);
            }
            esp_err_t send_err = httpd_resp_send(req, (const char *)inline_buffer, (ssize_t)length);
            free(inline_buffer);
            return send_err;
        }
    }

    uint8_t *buffer = heap_caps_malloc(EPHOTO_HTTP_STREAM_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = malloc(EPHOTO_HTTP_STREAM_CHUNK);
    }
    if (!buffer) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    bool read_to_end = length == 0;
    size_t remaining = length;
    while (read_to_end || remaining > 0) {
        size_t to_read =
            read_to_end || remaining > EPHOTO_HTTP_STREAM_CHUNK ? EPHOTO_HTTP_STREAM_CHUNK : remaining;
        size_t bytes_read = fread(buffer, 1, to_read, fp);
        if (bytes_read == 0) {
            if (feof(fp)) {
                break;
            }
            fclose(fp);
            free(buffer);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_send(req, NULL, 0);
        }
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)buffer, bytes_read);
        if (err != ESP_OK) {
            fclose(fp);
            free(buffer);
            return err;
        }
        if (!read_to_end) {
            remaining -= bytes_read;
        }
    }

    fclose(fp);
    free(buffer);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t send_screen_cached_photo_jpeg(httpd_req_t *req,
                                               const char *source_path,
                                               ephoto_fit_mode_t fit_mode,
                                               uint16_t rotation_deg)
{
    char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    size_t payload_offset = 0;
    size_t payload_size = 0;
    esp_err_t err = display_service_get_cache_jpeg_info(source_path,
                                                        fit_mode,
                                                        rotation_deg,
                                                        cache_path,
                                                        sizeof(cache_path),
                                                        &payload_offset,
                                                        &payload_size);
    if (err != ESP_OK || payload_size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, NULL, 0);
    }

    return send_file_chunked(req,
                             "image/jpeg",
                             cache_path,
                             payload_offset,
                             payload_size,
                             "private, max-age=120",
                             false);
}

static esp_err_t send_web_thumbnail_jpeg(httpd_req_t *req,
                                         const char *source_path,
                                         ephoto_fit_mode_t fit_mode)
{
    char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    size_t payload_offset = 0;
    size_t payload_size = 0;
    esp_err_t err = display_service_get_web_thumbnail_jpeg_info(source_path,
                                                                fit_mode,
                                                                cache_path,
                                                                sizeof(cache_path),
                                                                &payload_offset,
                                                                &payload_size);
    if (err != ESP_OK || payload_size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, NULL, 0);
    }

    return send_file_chunked(req,
                             "image/jpeg",
                             cache_path,
                             payload_offset,
                             payload_size,
                             "private, max-age=120",
                             false);
}

static esp_err_t enqueue_simple_command(ephoto_command_type_t type, int32_t value)
{
    ephoto_command_t cmd = {.type = type, .value_i32 = value};
    if (!s_command_handler) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_command_handler(&cmd);
}

static esp_err_t enqueue_uploaded_photo_path(const char *path)
{
    if (!s_command_handler || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    ephoto_command_t cmd = {.type = EPHOTO_CMD_QUEUE_UPLOADED_PHOTO};
    strlcpy(cmd.path, path, sizeof(cmd.path));
    return s_command_handler(&cmd);
}

static esp_err_t enqueue_delete_photo_path(const char *path)
{
    if (!s_command_handler || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    ephoto_command_t cmd = {.type = EPHOTO_CMD_DELETE_PHOTO_PATH};
    strlcpy(cmd.path, path, sizeof(cmd.path));
    return s_command_handler(&cmd);
}

static esp_err_t enqueue_upload_finalize_command(const char *latest_path, const char *message)
{
    if (!s_command_handler) {
        return ESP_ERR_INVALID_STATE;
    }
    ephoto_command_t cmd = {.type = EPHOTO_CMD_FINALIZE_UPLOAD_BATCH};
    strlcpy(cmd.text, message ? message : "", sizeof(cmd.text));
    strlcpy(cmd.path, latest_path ? latest_path : "", sizeof(cmd.path));
    return s_command_handler(&cmd);
}

static esp_err_t enqueue_notification(ephoto_notification_level_t level, const char *text)
{
    if (!s_command_handler) {
        return ESP_ERR_INVALID_STATE;
    }
    ephoto_command_t cmd = {
        .type = EPHOTO_CMD_SHOW_NOTIFICATION,
        .level = level,
    };
    strlcpy(cmd.text, text ? text : "", sizeof(cmd.text));
    return s_command_handler(&cmd);
}

static esp_err_t send_command_busy(httpd_req_t *req, esp_err_t err, const char *message)
{
    return send_result(req, false, message ? message : "系统正忙，请稍后重试", err == ESP_OK ? -40 : err);
}

static void populate_status_json(cJSON *json, const ephoto_app_state_t *snapshot)
{
    char now[32];
    char preview_url[EPHOTO_MAX_PHOTO_PATH_LEN * 3 + 64] = {0};
    char screen_on_time[6] = {0};
    char screen_off_time[6] = {0};
    clock_service_status_t clock_status = {0};
    upload_status_snapshot_t upload_status = {0};
    clock_service_get_time_string(now, sizeof(now));
    clock_service_get_status(&clock_status);
    upload_status_snapshot(&upload_status);
    const char *hostname = wifi_admin_get_hostname();
    const char *device_name = wifi_admin_get_device_name();
    const char *ap_ip = wifi_admin_get_ap_ip();
    const char *sta_ip = wifi_admin_get_sta_ip();
    const char *ap_ssid = wifi_admin_get_ap_ssid();
    const char *ap_password = wifi_admin_get_ap_password();
    ephoto_photo_t current_photo_meta = {0};
    bool have_current_photo_meta = false;

    cJSON *settings = cJSON_AddObjectToObject(json, "settings");
    cJSON_AddStringToObject(settings, "playbackMode", ephoto_playback_mode_to_string(snapshot->settings.playback_mode));
    cJSON_AddStringToObject(settings, "slideshowInterval", ephoto_interval_to_string(snapshot->settings.slideshow_interval));
    cJSON_AddStringToObject(settings, "fitMode", ephoto_fit_mode_to_string(snapshot->settings.fit_mode));
    cJSON_AddStringToObject(settings, "orientationFilter", ephoto_orientation_filter_to_string(snapshot->settings.orientation_filter));
    cJSON_AddStringToObject(settings, "brightnessMode", ephoto_brightness_mode_to_string(snapshot->settings.brightness_mode));
    cJSON_AddNumberToObject(settings, "brightnessPercent", snapshot->settings.brightness);
    cJSON_AddNumberToObject(settings, "manualBrightnessPercent", snapshot->settings.manual_brightness);
    cJSON_AddBoolToObject(settings, "clockVisible", snapshot->settings.clock_visible);
    cJSON_AddStringToObject(settings, "clockFormat", ephoto_clock_format_to_string(snapshot->settings.clock_format));
    cJSON_AddStringToObject(settings, "clockPosition", ephoto_clock_position_to_string(snapshot->settings.clock_position));
    cJSON_AddStringToObject(settings, "clockColor", ephoto_clock_color_to_string(snapshot->settings.clock_color));
    cJSON_AddStringToObject(settings, "otaInterval", ephoto_ota_interval_to_string(snapshot->settings.ota_interval));
    cJSON_AddStringToObject(settings, "otaChannel", snapshot->settings.ota_channel);
    cJSON_AddBoolToObject(settings, "screenOn", snapshot->settings.screen_on);
    cJSON_AddBoolToObject(settings, "screenScheduleEnabled", snapshot->settings.screen_schedule_enabled);
    cJSON_AddBoolToObject(settings, "autoTimeSync", snapshot->settings.auto_time_sync);
    cJSON_AddNumberToObject(settings, "rotationDeg", snapshot->settings.rotation_deg);
    cJSON_AddNumberToObject(settings, "manualRotationDeg", snapshot->settings.manual_rotation_deg);
    cJSON_AddBoolToObject(settings, "autoRotationEnabled", snapshot->settings.auto_rotation_enabled);
    cJSON_AddStringToObject(settings, "timezone", snapshot->settings.timezone);
    format_hhmm_string(snapshot->settings.screen_on_minute, screen_on_time, sizeof(screen_on_time));
    format_hhmm_string(snapshot->settings.screen_off_minute, screen_off_time, sizeof(screen_off_time));
    cJSON_AddStringToObject(settings, "screenOnTime", screen_on_time);
    cJSON_AddStringToObject(settings, "screenOffTime", screen_off_time);
    cJSON_AddNumberToObject(settings, "ambientLightMv", snapshot->ambient_light_raw_mv);
    cJSON_AddNumberToObject(settings, "ambientLightPercent", snapshot->ambient_light_percent);

    cJSON *storage = cJSON_AddObjectToObject(json, "storage");
    cJSON_AddBoolToObject(storage, "mounted", snapshot->storage.mounted);
    cJSON_AddBoolToObject(storage, "present", snapshot->storage.present);
    cJSON_AddNumberToObject(storage, "capacityBytes", (double)snapshot->storage.capacity_bytes);
    cJSON_AddNumberToObject(storage, "usedBytes", (double)snapshot->storage.used_bytes);
    cJSON_AddNumberToObject(storage, "photoCount", snapshot->storage.photo_count);
    cJSON_AddNumberToObject(storage, "imageCandidates", (double)gallery_service_get_total_image_candidates());
    cJSON_AddNumberToObject(storage, "unsupportedPngCount", (double)gallery_service_get_unsupported_png_count());
    cJSON_AddNumberToObject(storage, "unsupportedProgressiveJpegCount",
                            (double)gallery_service_get_unsupported_progressive_jpeg_count());
    cJSON_AddNumberToObject(storage, "unsupportedOtherCount", (double)gallery_service_get_unsupported_other_count());

    cJSON *notification = cJSON_AddObjectToObject(json, "notification");
    cJSON_AddStringToObject(notification, "level", ephoto_notification_level_to_string(snapshot->notification.level));
    cJSON_AddStringToObject(notification, "text", snapshot->notification.text);
    cJSON_AddNumberToObject(notification, "updatedAtMs", (double)snapshot->notification.updated_at_ms);

    cJSON *upload = cJSON_AddObjectToObject(json, "upload");
    cJSON_AddBoolToObject(upload, "active", upload_status.active);
    cJSON_AddBoolToObject(upload, "recoveryPending", upload_status.recovery_pending);
    cJSON_AddNumberToObject(upload, "sessionStartedAt", (double)upload_status.session_started_at_ms);
    cJSON_AddNumberToObject(upload, "completedCount", (double)upload_status.completed_count);
    cJSON_AddBoolToObject(upload, "recoveredByWatchdog", upload_status.recovered_by_watchdog);
    cJSON_AddStringToObject(upload, "lastRecoveryMessage", upload_status.last_recovery_message);

    cJSON_AddStringToObject(json, "networkMode", ephoto_network_mode_to_string(snapshot->network_mode));
    cJSON_AddStringToObject(json, "currentSsid", snapshot->current_ssid);
    cJSON_AddStringToObject(json, "currentPhotoName", snapshot->current_photo_name);
    cJSON_AddStringToObject(json, "currentPhotoPath", snapshot->current_photo_path);
    cJSON_AddStringToObject(json, "deviceName", device_name ? device_name : "");
    cJSON_AddStringToObject(json, "firmwareVersion", firmware_version_string());
    cJSON *ota = cJSON_AddObjectToObject(json, "ota");
    cJSON_AddBoolToObject(ota, "configured", snapshot->ota.configured);
    cJSON_AddBoolToObject(ota, "checking", snapshot->ota.checking);
    cJSON_AddBoolToObject(ota, "updateAvailable", snapshot->ota.update_available);
    cJSON_AddBoolToObject(ota, "updating", snapshot->ota.updating);
    cJSON_AddBoolToObject(ota, "rebootRequired", snapshot->ota.reboot_required);
    cJSON_AddBoolToObject(ota, "lastCheckOk", snapshot->ota.last_check_ok);
    cJSON_AddNumberToObject(ota, "progressPercent", snapshot->ota.progress_percent);
    cJSON_AddStringToObject(ota, "stage", ephoto_ota_stage_to_string(snapshot->ota.stage));
    cJSON_AddStringToObject(ota, "currentVersion", snapshot->ota.current_version);
    cJSON_AddStringToObject(ota, "availableVersion", snapshot->ota.available_version);
    cJSON_AddStringToObject(ota, "message", snapshot->ota.message);
    cJSON_AddStringToObject(ota, "releaseNotes", snapshot->ota.release_notes);
    cJSON_AddNumberToObject(ota, "lastError", snapshot->ota.last_error);
    cJSON_AddNumberToObject(ota, "lastCheckMs", (double)snapshot->ota.last_check_ms);
    cJSON_AddNumberToObject(ota, "lastUpdateMs", (double)snapshot->ota.last_update_ms);
    cJSON *boot_image = cJSON_AddObjectToObject(json, "bootImage");
    cJSON_AddBoolToObject(boot_image, "customAvailable", snapshot->boot_image.custom_available);
    cJSON_AddBoolToObject(boot_image, "customSelected", snapshot->boot_image.custom_selected);
    cJSON_AddBoolToObject(boot_image, "updateInProgress", snapshot->boot_image.update_in_progress);
    cJSON_AddStringToObject(boot_image,
                            "activeSource",
                            snapshot->boot_image.active_source == EPHOTO_BOOT_IMAGE_SOURCE_CUSTOM ? "custom" : "default");
    cJSON_AddStringToObject(boot_image, "activeName", snapshot->boot_image.active_name);
    cJSON_AddNumberToObject(boot_image, "updatedAtMs", (double)snapshot->boot_image.updated_at_ms);
    cJSON_AddNumberToObject(boot_image, "lastError", snapshot->boot_image.last_error);
    cJSON_AddStringToObject(json, "webMode", web_mode_to_string(s_web_mode));
    cJSON_AddNumberToObject(json, "webModeUpdatedAtMs", (double)s_web_mode_updated_at_ms);
    bool current_photo_cached = snapshot->current_photo_path[0] &&
                                display_service_has_cache(snapshot->current_photo_path,
                                                          snapshot->settings.fit_mode,
                                                          snapshot->settings.rotation_deg);
    if (snapshot->current_photo_path[0]) {
        int current_index = gallery_service_find_index_by_path(snapshot->current_photo_path);
        if (current_index >= 0 &&
            gallery_service_get_item(current_index, &current_photo_meta) == ESP_OK) {
            have_current_photo_meta = true;
        }
    }
    if (current_photo_cached) {
        build_screen_preview_url(snapshot->current_photo_path,
                                 snapshot->settings.fit_mode,
                                 snapshot->settings.rotation_deg,
                                 have_current_photo_meta ? &current_photo_meta : NULL,
                                 preview_url,
                                 sizeof(preview_url));
    }
    cJSON_AddStringToObject(json, "currentPhotoPreviewUrl", preview_url);
    cJSON_AddBoolToObject(json, "interactionLocked", snapshot->interaction_locked);
    cJSON_AddBoolToObject(json, "cacheBuildActive", snapshot->cache_build_active);
    cJSON_AddNumberToObject(json, "cacheBuildTotal", (double)snapshot->cache_build_total);
    cJSON_AddNumberToObject(json, "cacheBuildDone", (double)snapshot->cache_build_done);
    cJSON_AddNumberToObject(json, "cacheBuildFailed", (double)snapshot->cache_build_failed);
    cJSON_AddNumberToObject(json, "cacheBuildPhotoTotal", (double)snapshot->cache_build_photo_total);
    cJSON_AddNumberToObject(json, "cacheBuildPhotoIndex", (double)snapshot->cache_build_photo_index);
    cJSON_AddStringToObject(json, "cacheBuildPhotoName", snapshot->cache_build_photo_name);
    cJSON_AddBoolToObject(json, "currentPhotoReady",
                          snapshot->current_photo_path[0] &&
                          display_service_is_photo_ready(snapshot->current_photo_path,
                                                         snapshot->settings.fit_mode,
                                                         snapshot->settings.rotation_deg));
    cJSON_AddBoolToObject(json, "currentPhotoCached", current_photo_cached);
    cJSON_AddStringToObject(json, "currentTime", now);
    cJSON *clock = cJSON_AddObjectToObject(json, "clock");
    cJSON_AddBoolToObject(clock, "timeValid", clock_status.time_valid);
    cJSON_AddBoolToObject(clock, "autoSyncEnabled", clock_status.auto_sync_enabled);
    cJSON_AddBoolToObject(clock, "syncInProgress", clock_status.sync_in_progress);
    cJSON_AddNumberToObject(clock, "lastSyncMs", (double)clock_status.last_sync_ms);
    cJSON_AddStringToObject(clock, "lastSyncTime", clock_status.last_sync_time);
    cJSON_AddStringToObject(clock, "lastSyncSource", clock_status.last_sync_source);
    cJSON_AddStringToObject(clock, "timezone", clock_status.timezone);
    cJSON_AddBoolToObject(json, "menuVisible", snapshot->menu_visible);
    cJSON *album = cJSON_AddObjectToObject(json, "album");
    cJSON_AddBoolToObject(album, "visible", snapshot->album.visible);
    cJSON_AddBoolToObject(album, "actionPopupVisible", snapshot->album.action_popup_visible);
    cJSON_AddBoolToObject(album, "actionConfirmPending", snapshot->album.action_confirm_pending);
    cJSON_AddNumberToObject(album, "slotCount", snapshot->album.slot_count);
    cJSON_AddNumberToObject(album, "selectedSlot", snapshot->album.selected_slot);
    cJSON_AddNumberToObject(album, "selectedAction", snapshot->album.selected_action);
    cJSON_AddNumberToObject(album, "actionCount", snapshot->album.action_count);
    cJSON_AddNumberToObject(album, "totalCount", (double)snapshot->album.total_count);
    cJSON_AddNumberToObject(album, "selectedPosition", (double)snapshot->album.selected_position);
    cJSON_AddStringToObject(album, "title", snapshot->album.title);
    cJSON_AddStringToObject(album, "summary", snapshot->album.summary);
    cJSON_AddStringToObject(album, "hint", snapshot->album.hint);
    cJSON_AddStringToObject(album, "actionTitle", snapshot->album.action_title);
    cJSON_AddNumberToObject(json, "ambientLightMv", snapshot->ambient_light_raw_mv);
    cJSON_AddNumberToObject(json, "ambientLightPercent", snapshot->ambient_light_percent);

    cJSON *wifi = cJSON_AddObjectToObject(json, "wifi");
    cJSON_AddStringToObject(wifi, "deviceName", device_name ? device_name : "");
    cJSON_AddStringToObject(wifi, "hostname", hostname ? hostname : "");
    cJSON_AddStringToObject(wifi, "apSsid", ap_ssid ? ap_ssid : "");
    cJSON_AddStringToObject(wifi, "apPassword", ap_password ? ap_password : "");
    cJSON_AddStringToObject(wifi, "apIp", ap_ip ? ap_ip : "");
    cJSON_AddStringToObject(wifi, "staIp", sta_ip ? sta_ip : "");
    cJSON_AddBoolToObject(wifi, "staConnected", wifi_admin_is_sta_connected());
    cJSON_AddNumberToObject(wifi, "staRssi", (double)wifi_admin_get_sta_rssi());
    if (ap_ip && ap_ip[0]) {
        char ap_url[64];
        snprintf(ap_url, sizeof(ap_url), "http://%s", ap_ip);
        cJSON_AddStringToObject(wifi, "apUrl", ap_url);
    } else {
        cJSON_AddStringToObject(wifi, "apUrl", "");
    }
    if (hostname && hostname[0]) {
        char mdns_url[64];
        snprintf(mdns_url, sizeof(mdns_url), "http://%s.local", hostname);
        cJSON_AddStringToObject(wifi, "mdnsUrl", mdns_url);
    } else {
        cJSON_AddStringToObject(wifi, "mdnsUrl", "");
    }
    if (sta_ip && sta_ip[0]) {
        char sta_url[64];
        snprintf(sta_url, sizeof(sta_url), "http://%s", sta_ip);
        cJSON_AddStringToObject(wifi, "staUrl", sta_url);
    } else {
        cJSON_AddStringToObject(wifi, "staUrl", "");
    }

    char host_version[24] = {0};
    char slave_version[24] = {0};
    snprintf(host_version,
             sizeof(host_version),
             "%lu.%lu.%lu",
             (unsigned long)snapshot->hosted.host_major,
             (unsigned long)snapshot->hosted.host_minor,
             (unsigned long)snapshot->hosted.host_patch);
    snprintf(slave_version,
             sizeof(slave_version),
             "%lu.%lu.%lu",
             (unsigned long)snapshot->hosted.slave_major,
             (unsigned long)snapshot->hosted.slave_minor,
             (unsigned long)snapshot->hosted.slave_patch);

    cJSON *hosted = cJSON_AddObjectToObject(json, "hosted");
    cJSON_AddBoolToObject(hosted, "available", snapshot->hosted.available);
    cJSON_AddBoolToObject(hosted, "queried", snapshot->hosted.queried);
    cJSON_AddBoolToObject(hosted, "needsUpgrade", snapshot->hosted.needs_upgrade);
    cJSON_AddBoolToObject(hosted, "slaveOtaSupported", snapshot->hosted.slave_ota_supported);
    cJSON_AddNumberToObject(hosted, "versionRelation", snapshot->hosted.version_relation);
    cJSON_AddNumberToObject(hosted, "lastQueryMs", (double)snapshot->hosted.last_query_ms);
    cJSON_AddNumberToObject(hosted, "lastError", snapshot->hosted.last_error);
    cJSON_AddStringToObject(hosted,
                            "lastErrorText",
                            snapshot->hosted.last_error == ESP_OK ? "ESP_OK" : esp_err_to_name(snapshot->hosted.last_error));
    cJSON_AddStringToObject(hosted, "hostVersion", host_version);
    cJSON_AddStringToObject(hosted, "slaveVersion", slave_version);

}

static esp_err_t handle_root(httpd_req_t *req)
{
    return send_embedded_text(req, "text/html; charset=utf-8", index_html_start, index_html_end);
}

static esp_err_t handle_css(httpd_req_t *req)
{
    return send_embedded_text(req, "text/css; charset=utf-8", style_css_start, style_css_end);
}

static esp_err_t handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    ephoto_app_state_t *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        return send_result(req, false, "内存不足", -12);
    }
    s_snapshot_fn(snapshot);
    cJSON *json = cJSON_CreateObject();
    populate_status_json(json, snapshot);
    free(snapshot);
    return send_json_response(req, json);
}

static esp_err_t handle_storage(httpd_req_t *req)
{
    ephoto_storage_status_t status = {0};
    storage_service_get_status(&status);
    int photo_count = gallery_service_get_count();
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "mounted", status.mounted);
    cJSON_AddBoolToObject(json, "present", status.present);
    cJSON_AddNumberToObject(json, "capacityBytes", (double)status.capacity_bytes);
    cJSON_AddNumberToObject(json, "usedBytes", (double)status.used_bytes);
    cJSON_AddNumberToObject(json, "photoCount", photo_count >= 0 ? photo_count : 0);
    cJSON_AddNumberToObject(json, "imageCandidates", (double)gallery_service_get_total_image_candidates());
    cJSON_AddNumberToObject(json, "unsupportedPngCount", (double)gallery_service_get_unsupported_png_count());
    cJSON_AddNumberToObject(json, "unsupportedProgressiveJpegCount", (double)gallery_service_get_unsupported_progressive_jpeg_count());
    cJSON_AddNumberToObject(json, "unsupportedOtherCount", (double)gallery_service_get_unsupported_other_count());
    cJSON_AddNumberToObject(json, "busWidth", storage_service_get_mounted_bus_width());
    cJSON_AddNumberToObject(json, "freqKHz", storage_service_get_mounted_freq_khz());
    cJSON_AddStringToObject(json, "mountPath", storage_service_get_mount_path());
    return send_json_response(req, json);
}

static esp_err_t handle_storage_benchmark(httpd_req_t *req)
{
    // A benchmark writes and reads several megabytes synchronously. It can
    // starve album requests long enough for mobile clients to time out.
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddBoolToObject(json, "enabled", false);
    cJSON_AddStringToObject(json, "message", "TF测速已禁用，不会读写TF卡");
    cJSON_AddNumberToObject(json, "bytesTested", 0);
    cJSON_AddNumberToObject(json, "writeMs", 0);
    cJSON_AddNumberToObject(json, "readMs", 0);
    cJSON_AddNumberToObject(json, "writeKiBps", 0);
    cJSON_AddNumberToObject(json, "readKiBps", 0);
    cJSON_AddNumberToObject(json, "busWidth", storage_service_get_mounted_bus_width());
    cJSON_AddNumberToObject(json, "freqKHz", storage_service_get_mounted_freq_khz());
    cJSON_AddNumberToObject(json, "errorCode", 0);
    cJSON_AddStringToObject(json, "errorText", "ESP_OK");
    return send_json_response(req, json);
}

static size_t parse_photo_page_query_value(const char *value, size_t default_value, size_t maximum)
{
    char *end = NULL;
    unsigned long parsed = 0;
    if (!value || !value[0] || value[0] == '-') {
        return default_value;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed > maximum) {
        return default_value;
    }
    return (size_t)parsed;
}

static esp_err_t handle_photos(httpd_req_t *req)
{
    if (web_mode_blocks_album_io()) {
        return send_result(req, false, "上传模式进行中，相册数据已暂停", -42);
    }
    char query[64] = {0};
    char offset_value[16] = {0};
    char limit_value[16] = {0};
    size_t offset = 0;
    size_t limit = EPHOTO_PHOTO_PAGE_DEFAULT_LIMIT;
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "offset", offset_value, sizeof(offset_value)) == ESP_OK) {
            offset = parse_photo_page_query_value(offset_value, 0, EPHOTO_PHOTO_PAGE_MAX_OFFSET);
        }
        if (httpd_query_key_value(query, "limit", limit_value, sizeof(limit_value)) == ESP_OK) {
            limit = parse_photo_page_query_value(limit_value,
                                                 EPHOTO_PHOTO_PAGE_DEFAULT_LIMIT,
                                                 EPHOTO_PHOTO_PAGE_MAX_LIMIT);
            if (limit == 0) {
                limit = EPHOTO_PHOTO_PAGE_DEFAULT_LIMIT;
            }
        }
    }

    ephoto_photo_t *items = NULL;
    size_t count = 0;
    size_t total = 0;
    esp_err_t range_err = gallery_service_get_range(offset, limit, &items, &count, &total);
    if (range_err != ESP_OK) {
        return send_result(req, false, "读取相册分页数据失败", range_err);
    }
    // The collection can shrink between page requests (for example after a
    // deletion from another client). Return the last valid page instead of a
    // confusing empty page with a valid nonzero total.
    if (total > 0 && offset >= total) {
        offset = ((total - 1U) / limit) * limit;
        gallery_service_release_all(items);
        items = NULL;
        count = 0;
        range_err = gallery_service_get_range(offset, limit, &items, &count, &total);
        if (range_err != ESP_OK) {
            return send_result(req, false, "读取相册分页数据失败", range_err);
        }
    }
    cJSON *json = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(json, "items");
    cJSON_AddNumberToObject(json, "total", (double)total);
    cJSON_AddNumberToObject(json, "offset", (double)offset);
    cJSON_AddNumberToObject(json, "limit", (double)limit);
    cJSON_AddBoolToObject(json, "hasMore", offset + count < total);
    for (size_t i = 0; i < count; ++i) {
        char thumb_url[EPHOTO_MAX_PHOTO_PATH_LEN * 3 + 64] = {0};
        char preview_url[EPHOTO_MAX_PHOTO_PATH_LEN * 3 + 64] = {0};
        // Do not probe thumbnail files here. With a large album that becomes
        // hundreds of synchronous TF-card stats and blocks the HTTP server.
        // The web UI requests only visible thumbnails with two-way concurrency;
        // the thumbnail endpoint remains the authoritative cache check.
        build_web_thumbnail_url(items[i].path, EPHOTO_FIT_COVER, &items[i], thumb_url, sizeof(thumb_url));
        build_web_thumbnail_url(items[i].path, EPHOTO_FIT_CONTAIN, &items[i], preview_url, sizeof(preview_url));
        bool has_thumbnail_url = thumb_url[0] != '\0';
        bool has_preview_url = preview_url[0] != '\0';
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", (double)(offset + i));
        cJSON_AddStringToObject(item, "name", items[i].name);
        cJSON_AddStringToObject(item, "path", items[i].path);
        cJSON_AddStringToObject(item, "thumbnailUrl", thumb_url);
        cJSON_AddStringToObject(item, "previewUrl", preview_url);
        cJSON_AddBoolToObject(item, "thumbnailReady", has_thumbnail_url);
        cJSON_AddBoolToObject(item, "thumbnailFailed", false);
        cJSON_AddBoolToObject(item, "previewReady", has_preview_url);
        cJSON_AddBoolToObject(item, "previewFailed", false);
        cJSON_AddNumberToObject(item, "thumbnailErrorCode", 0);
        cJSON_AddStringToObject(item, "thumbnailErrorText", "");
        cJSON_AddNumberToObject(item, "previewErrorCode", 0);
        cJSON_AddStringToObject(item, "previewErrorText", "");
        cJSON_AddNumberToObject(item, "sizeBytes", (double)items[i].size_bytes);
        cJSON_AddNumberToObject(item, "mtime", (double)items[i].mtime);
        cJSON_AddNumberToObject(item, "rawWidth", items[i].raw_width);
        cJSON_AddNumberToObject(item, "rawHeight", items[i].raw_height);
        cJSON_AddNumberToObject(item, "width", items[i].width);
        cJSON_AddNumberToObject(item, "height", items[i].height);
        cJSON_AddNumberToObject(item, "exifOrientation", items[i].exif_orientation);
        cJSON_AddNumberToObject(item, "effectiveRotationDeg", items[i].effective_rotation_deg);
        cJSON_AddNumberToObject(item, "manualRotationDeg", items[i].manual_rotation_deg);
        cJSON_AddBoolToObject(item, "canAdjustOrientation", photo_supports_orientation_adjustment_web(&items[i]));
        cJSON_AddStringToObject(item,
                                "orientation",
                                items[i].width >= items[i].height ? "landscape" : "portrait");
        cJSON_AddItemToArray(array, item);
    }
    gallery_service_release_all(items);
    return send_json_response(req, json);
}

static esp_err_t handle_thumb(httpd_req_t *req)
{
    if (web_mode_blocks_album_io()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, NULL, 0);
    }
    const char *encoded_path = req->uri + strlen("/api/v1/thumb/");
    const char *query_sep = strchr(encoded_path, '?');
    size_t encoded_len = query_sep ? (size_t)(query_sep - encoded_path) : strlen(encoded_path);
    char encoded_path_only[EPHOTO_MAX_PHOTO_PATH_LEN * 3] = {0};
    char path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    char query[64] = {0};
    char fit_value[16] = {0};
    char rotation_value[8] = {0};
    char asset_value[16] = {0};
    ephoto_fit_mode_t fit_mode = EPHOTO_FIT_CONTAIN;
    uint16_t rotation_deg = 0;
    bool screen_asset = false;

    if (encoded_len == 0 || encoded_len >= sizeof(encoded_path_only)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, NULL, 0);
    }
    memcpy(encoded_path_only, encoded_path, encoded_len);
    encoded_path_only[encoded_len] = '\0';

    if (!decode_uri_component(encoded_path_only, path, sizeof(path))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, NULL, 0);
    }
    if (!photo_path_is_valid_gallery_item(path)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, NULL, 0);
    }

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "fit", fit_value, sizeof(fit_value)) == ESP_OK) {
            fit_mode = parse_fit_mode(fit_value);
        }
        if (httpd_query_key_value(query, "r", rotation_value, sizeof(rotation_value)) == ESP_OK) {
            rotation_deg = atoi(rotation_value) == 90 ? 90 : 0;
        }
        if (httpd_query_key_value(query, "asset", asset_value, sizeof(asset_value)) == ESP_OK) {
            screen_asset = strcmp(asset_value, "screen") == 0;
        }
    }

    return screen_asset
               ? send_screen_cached_photo_jpeg(req, path, fit_mode, rotation_deg)
               : send_web_thumbnail_jpeg(req, path, fit_mode);
}

static bool is_safe_filename(const char *filename)
{
    if (!filename || !filename[0]) {
        return false;
    }

    if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0 || strstr(filename, "..")) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)filename; *p; ++p) {
        if (*p < 0x20) {
            return false;
        }
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' ||
            *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            return false;
        }
    }
    return true;
}

static esp_err_t handle_upload_inner(httpd_req_t *req)
{
    set_web_mode(EPHOTO_WEB_MODE_UPLOAD);
    // Keep a previously completed batch open while the next file is arriving.
    upload_finalize_watchdog_touch();
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许上传", -20);
    }
    char encoded_name[EPHOTO_MAX_ENCODED_NAME_LEN] = {0};
    char filename[EPHOTO_MAX_PHOTO_NAME_LEN] = {0};
    char defer_cache_value[8] = {0};
    char content_type[128] = {0};
    char multipart_boundary[EPHOTO_MULTIPART_BOUNDARY_MAX_LEN] = {0};
    bool defer_cache_build = false;
    bool filename_loaded = false;
    bool is_multipart = false;

    size_t encoded_name_len = httpd_req_get_hdr_value_len(req, "X-Filename-B64");
    if (encoded_name_len > 0) {
        if (encoded_name_len >= sizeof(encoded_name)) {
            return send_result(req, false, "文件名过长，请改短后再上传", -2);
        }
        if (httpd_req_get_hdr_value_str(req, "X-Filename-B64", encoded_name, sizeof(encoded_name)) != ESP_OK) {
            return send_result(req, false, "读取文件名失败", -3);
        }
        if (!decode_base64_filename(encoded_name, filename, sizeof(filename))) {
            return send_result(req, false, "文件名解析失败，请使用更短或更简单的文件名", -4);
        }
        filename_loaded = filename[0] != '\0';
    }

    if (!filename_loaded) {
        encoded_name_len = httpd_req_get_hdr_value_len(req, "X-Filename");
        if (encoded_name_len > 0) {
            if (encoded_name_len >= sizeof(encoded_name)) {
                return send_result(req, false, "文件名过长，请改短后再上传", -2);
            }
            if (httpd_req_get_hdr_value_str(req, "X-Filename", encoded_name, sizeof(encoded_name)) != ESP_OK) {
                return send_result(req, false, "读取文件名失败", -3);
            }
            filename_loaded = true;
        }
    }

    if (!filename_loaded) {
        char query[EPHOTO_MAX_ENCODED_NAME_LEN + 32] = {0};
        if (httpd_req_get_url_query_len(req) > 0 &&
            httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "filename", encoded_name, sizeof(encoded_name)) == ESP_OK &&
            encoded_name[0] != '\0') {
            filename_loaded = true;
        }
    }

    if (!filename_loaded) {
        return send_result(req, false, "缺少文件名，请重新选择照片后上传", -1);
    }
    if (httpd_req_get_hdr_value_str(req, "X-Defer-Cache", defer_cache_value, sizeof(defer_cache_value)) == ESP_OK) {
        defer_cache_build = strcmp(defer_cache_value, "1") == 0;
    }
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        is_multipart = parse_multipart_boundary(content_type, multipart_boundary, sizeof(multipart_boundary));
    }

    if (filename_loaded && filename[0] == '\0') {
        if (!decode_uri_component(encoded_name, filename, sizeof(filename))) {
            return send_result(req, false, "文件名解析失败，请使用更短或更简单的文件名", -4);
        }
    }
    if (!is_safe_filename(filename)) {
        return send_result(req, false, "文件名不合法", -5);
    }
    if (!has_supported_upload_extension(filename)) {
        return send_result(req,
                           false,
                           "不支持该文件类型。当前仅支持 JPEG / PNG / BMP 图片，请重新选择照片后上传。",
                           -16);
    }

    char path[EPHOTO_MAX_PHOTO_PATH_LEN];
    int path_len = snprintf(path, sizeof(path), "%s/%s", storage_service_get_photo_dir(), filename);
    if (path_len < 0 || path_len >= (int)sizeof(path)) {
        return send_result(req, false, "文件名过长，请改短后再上传", -6);
    }
    char temp_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    int temp_len = snprintf(temp_path, sizeof(temp_path), "%s.uploading", path);
    if (temp_len < 0 || temp_len >= (int)sizeof(temp_path)) {
        return send_result(req, false, "临时文件名过长", -11);
    }
    char backup_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    int backup_len = snprintf(backup_path, sizeof(backup_path), "%s.backup", path);
    if (backup_len < 0 || backup_len >= (int)sizeof(backup_path)) {
        return send_result(req, false, "备份文件名过长", -13);
    }

    FILE *fp = fopen(temp_path, "wb");
    if (!fp) {
        return send_result(req, false, "无法写入 SD 卡", -7);
    }

    char *recv_buffer = heap_caps_malloc(EPHOTO_UPLOAD_IO_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recv_buffer) {
        recv_buffer = malloc(EPHOTO_UPLOAD_IO_CHUNK);
    }
    char *file_buffer = NULL;
    size_t sd_write_buffer_size = 0;
    char *sd_write_buffer = (char *)allocate_upload_dma_buffer(&sd_write_buffer_size);
    size_t sd_write_pending = 0;
    size_t multipart_marker_len = 0;
    char multipart_marker[EPHOTO_MULTIPART_BOUNDARY_MAX_LEN + 8] = {0};
    char multipart_header[EPHOTO_MULTIPART_HEADER_MAX_LEN] = {0};
    size_t multipart_header_len = 0;
    bool multipart_header_done = !is_multipart;
    bool multipart_finished = false;
    size_t multipart_tail_len = 0;
    uint8_t multipart_tail[EPHOTO_MULTIPART_BOUNDARY_MAX_LEN + 8] = {0};
    uint8_t *multipart_scan_buffer = NULL;
    if (!recv_buffer || !sd_write_buffer) {
        fclose(fp);
        unlink(temp_path);
        free(file_buffer);
        free(sd_write_buffer);
        return send_result(req, false, "内存不足，无法上传", -10);
    }
    if (is_multipart) {
        int written = snprintf(multipart_marker, sizeof(multipart_marker), "\r\n--%s", multipart_boundary);
        if (written <= 0 || written >= (int)sizeof(multipart_marker)) {
            fclose(fp);
            free(file_buffer);
            free(sd_write_buffer);
            free(recv_buffer);
            unlink(temp_path);
            return send_result(req, false, "上传边界解析失败", -18);
        }
        multipart_marker_len = (size_t)written;
        multipart_scan_buffer = heap_caps_malloc(EPHOTO_UPLOAD_IO_CHUNK + multipart_marker_len + 8,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!multipart_scan_buffer) {
            multipart_scan_buffer = malloc(EPHOTO_UPLOAD_IO_CHUNK + multipart_marker_len + 8);
        }
        if (!multipart_scan_buffer) {
            fclose(fp);
            free(file_buffer);
            free(sd_write_buffer);
            free(recv_buffer);
            unlink(temp_path);
            return send_result(req, false, "内存不足，无法解析上传内容", -10);
        }
    }

    int remaining = req->content_len;
    int total_size = remaining;
    int payload_size = 0;
    int64_t started_ms = clock_service_now_ms();
    int64_t receive_done_ms = started_ms;
    int64_t commit_done_ms = started_ms;
    int64_t queue_done_ms = started_ms;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req,
                                      recv_buffer,
                                      remaining > EPHOTO_UPLOAD_IO_CHUNK ? EPHOTO_UPLOAD_IO_CHUNK : remaining);
        if (recv_len <= 0) {
            int received = total_size - remaining;
            char message[96];
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                snprintf(message,
                         sizeof(message),
                         "上传超时，已接收 %d/%d KB",
                         received / 1024,
                         (total_size + 1023) / 1024);
                ESP_LOGW(TAG,
                         "upload timeout: %s received=%d/%d bytes",
                         filename,
                         received,
                         total_size);
            }
            else {
                snprintf(message,
                         sizeof(message),
                         "上传中断，已接收 %d/%d KB",
                         received / 1024,
                         (total_size + 1023) / 1024);
                ESP_LOGW(TAG,
                         "upload interrupted: %s recv=%d received=%d/%d bytes",
                         filename,
                         recv_len,
                         received,
                         total_size);
            }
            fclose(fp);
            free(file_buffer);
            free(sd_write_buffer);
            free(recv_buffer);
            free(multipart_scan_buffer);
            unlink(temp_path);
            return send_result(req, false, message, recv_len == HTTPD_SOCK_ERR_TIMEOUT ? -15 : -8);
        }
        upload_finalize_watchdog_touch();
        if (!is_multipart) {
            if (!write_upload_chunk(fp,
                                    (uint8_t *)sd_write_buffer,
                                    sd_write_buffer_size,
                                    &sd_write_pending,
                                    recv_buffer,
                                    (size_t)recv_len)) {
                fclose(fp);
                free(file_buffer);
                free(sd_write_buffer);
                free(recv_buffer);
                free(multipart_scan_buffer);
                unlink(temp_path);
                return send_result(req, false, "写入文件失败", -9);
            }
            payload_size += recv_len;
        } else if (!multipart_finished) {
            const uint8_t *chunk_data = (const uint8_t *)recv_buffer;
            size_t chunk_len = (size_t)recv_len;

            if (!multipart_header_done) {
                if (multipart_header_len + chunk_len > sizeof(multipart_header)) {
                    fclose(fp);
                    free(file_buffer);
                    free(sd_write_buffer);
                    free(recv_buffer);
                    free(multipart_scan_buffer);
                    unlink(temp_path);
                    return send_result(req, false, "上传表单头过长，请重试", -18);
                }
                memcpy(multipart_header + multipart_header_len, chunk_data, chunk_len);
                multipart_header_len += chunk_len;
                const uint8_t *header_end =
                    find_bytes((const uint8_t *)multipart_header, multipart_header_len, "\r\n\r\n", 4);
                if (!header_end) {
                    remaining -= recv_len;
                    continue;
                }

                size_t header_block_len =
                    (size_t)((const char *)header_end - multipart_header) + 4U;
                size_t initial_data_len = multipart_header_len - header_block_len;

                if (strncmp(multipart_header, "--", 2) != 0 ||
                    strstr(multipart_header, multipart_boundary) == NULL) {
                    fclose(fp);
                    free(file_buffer);
                    free(sd_write_buffer);
                    free(recv_buffer);
                    free(multipart_scan_buffer);
                    unlink(temp_path);
                    return send_result(req, false, "上传表单格式不正确", -18);
                }

                multipart_header_done = true;
                chunk_data = (const uint8_t *)multipart_header + header_block_len;
                chunk_len = initial_data_len;
            }

            if (chunk_len > 0) {
                memcpy(multipart_scan_buffer, multipart_tail, multipart_tail_len);
                memcpy(multipart_scan_buffer + multipart_tail_len, chunk_data, chunk_len);
                size_t combined_len = multipart_tail_len + chunk_len;
                const uint8_t *marker =
                    find_bytes(multipart_scan_buffer, combined_len, multipart_marker, multipart_marker_len);

                if (marker) {
                    size_t flush_len = (size_t)(marker - multipart_scan_buffer);
                    if (flush_len > 0 &&
                        !write_upload_chunk(fp,
                                            (uint8_t *)sd_write_buffer,
                                            sd_write_buffer_size,
                                            &sd_write_pending,
                                            multipart_scan_buffer,
                                            flush_len)) {
                        fclose(fp);
                        free(file_buffer);
                        free(sd_write_buffer);
                        free(recv_buffer);
                        free(multipart_scan_buffer);
                        unlink(temp_path);
                        return send_result(req, false, "写入文件失败", -9);
                    }
                    payload_size += (int)flush_len;
                    multipart_finished = true;
                    multipart_tail_len = 0;
                } else if (combined_len > (multipart_marker_len - 1U)) {
                    size_t keep_len = multipart_marker_len - 1U;
                    size_t flush_len = combined_len - keep_len;
                    if (flush_len > 0 &&
                        !write_upload_chunk(fp,
                                            (uint8_t *)sd_write_buffer,
                                            sd_write_buffer_size,
                                            &sd_write_pending,
                                            multipart_scan_buffer,
                                            flush_len)) {
                        fclose(fp);
                        free(file_buffer);
                        free(sd_write_buffer);
                        free(recv_buffer);
                        free(multipart_scan_buffer);
                        unlink(temp_path);
                        return send_result(req, false, "写入文件失败", -9);
                    }
                    payload_size += (int)flush_len;
                    memcpy(multipart_tail, multipart_scan_buffer + flush_len, keep_len);
                    multipart_tail_len = keep_len;
                } else {
                    memcpy(multipart_tail, multipart_scan_buffer, combined_len);
                    multipart_tail_len = combined_len;
                }
            }
        }
        remaining -= recv_len;
    }
    if (is_multipart && (!multipart_header_done || !multipart_finished)) {
        fclose(fp);
        free(file_buffer);
        free(sd_write_buffer);
        free(recv_buffer);
        free(multipart_scan_buffer);
        unlink(temp_path);
        return send_result(req, false, "上传内容不完整，请重新上传", -18);
    }
    if (!flush_upload_chunk(fp, (uint8_t *)sd_write_buffer, &sd_write_pending)) {
        fclose(fp);
        free(file_buffer);
        free(sd_write_buffer);
        free(recv_buffer);
        free(multipart_scan_buffer);
        unlink(temp_path);
        return send_result(req, false, "写入文件失败", -9);
    }
    receive_done_ms = clock_service_now_ms();
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(multipart_scan_buffer);

    char validation_message[128] = {0};
    if (!validate_uploaded_temp_file(temp_path, filename, validation_message, sizeof(validation_message))) {
        free(file_buffer);
        free(sd_write_buffer);
        free(recv_buffer);
        unlink(temp_path);
        return send_result(req,
                           false,
                           validation_message[0] ? validation_message : "上传文件不是受支持的图片格式",
                           -17);
    }

    bool had_existing_file = access(path, F_OK) == 0;
    bool moved_existing_to_backup = false;
    if (had_existing_file) {
        unlink(backup_path);
        if (rename(path, backup_path) != 0) {
            unlink(temp_path);
            free(file_buffer);
            free(sd_write_buffer);
            free(recv_buffer);
            return send_result(req, false, "备份旧文件失败", -14);
        }
        moved_existing_to_backup = true;
    }
    if (rename(temp_path, path) != 0) {
        if (moved_existing_to_backup) {
            (void)rename(backup_path, path);
        }
        unlink(temp_path);
        free(file_buffer);
        free(sd_write_buffer);
        free(recv_buffer);
        return send_result(req, false, "保存文件失败", -12);
    }
    if (moved_existing_to_backup) {
        unlink(backup_path);
        // A same-name upload replaces the image, so discard the old image's
        // path-based rotation metadata only after the replacement is committed.
        esp_err_t rotation_err = gallery_service_reset_photo_orientation_by_path(path, NULL);
        if (rotation_err != ESP_OK && rotation_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "clear manual rotation for replaced photo failed: %s",
                     esp_err_to_name(rotation_err));
        }
    }
    if (had_existing_file) {
        display_service_invalidate_photo_cache(path);
    }
    commit_done_ms = clock_service_now_ms();
    free(file_buffer);
    free(sd_write_buffer);
    free(recv_buffer);
    upload_session_note_completed_file();

    int64_t total_ms = commit_done_ms - started_ms;
    int64_t receive_ms = receive_done_ms - started_ms;
    int64_t commit_ms = commit_done_ms - receive_done_ms;
    int64_t kbps = receive_ms > 0 ? ((int64_t)payload_size * 1000LL) / receive_ms / 1024LL : 0;
    ESP_LOGI(TAG,
             "upload saved: %s size=%d bytes receive=%lldms commit=%lldms total=%lldms speed=%lldKB/s defer=%d",
             path,
             payload_size,
             (long long)receive_ms,
             (long long)commit_ms,
             (long long)total_ms,
             (long long)kbps,
             defer_cache_build ? 1 : 0);

    if (defer_cache_build) {
        esp_err_t queue_path_err = enqueue_uploaded_photo_path(path);
        if (queue_path_err != ESP_OK) {
            return send_result(req, false, "照片已保存，但上传队列登记失败，请稍后刷新相册确认", queue_path_err);
        }
        upload_finalize_watchdog_record(path);
    } else {
        char cache_note[EPHOTO_MAX_NOTIFICATION_LEN];
        snprintf(cache_note,
                 sizeof(cache_note),
                 "正在生成缓存: %.100s",
                 filename);
        esp_err_t queue_err = enqueue_upload_finalize_command(path, cache_note);
        if (queue_err != ESP_OK) {
            return send_result(req, false, "照片已保存，但缓存任务排队失败，请稍后手动重建缓存", queue_err);
        }
        upload_finalize_watchdog_clear();
    }
    queue_done_ms = clock_service_now_ms();

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddStringToObject(json, "message", "上传成功");
    cJSON_AddNumberToObject(json, "code", 0);
    cJSON *profile = cJSON_AddObjectToObject(json, "profile");
    cJSON_AddStringToObject(profile, "filename", filename);
    cJSON_AddStringToObject(profile, "savedPath", path);
    cJSON_AddNumberToObject(profile, "bytes", (double)payload_size);
    cJSON_AddBoolToObject(profile, "deferCacheBuild", defer_cache_build);
    cJSON_AddNumberToObject(profile, "receiveMs", (double)receive_ms);
    cJSON_AddNumberToObject(profile, "commitMs", (double)commit_ms);
    cJSON_AddNumberToObject(profile, "queueMs", (double)(queue_done_ms - commit_done_ms));
    cJSON_AddNumberToObject(profile, "totalMs", (double)(queue_done_ms - started_ms));
    cJSON_AddNumberToObject(profile, "transferKiBps", (double)kbps);
    return send_json_response(req, json);
}

static esp_err_t handle_upload(httpd_req_t *req)
{
    if (!s_upload_write_mutex) {
        return send_result(req, false, "上传服务尚未就绪", -19);
    }
    if (xSemaphoreTake(s_upload_write_mutex, pdMS_TO_TICKS(120000)) != pdTRUE) {
        return send_result(req, false, "已有照片正在上传，请稍后重试", -20);
    }
    esp_err_t err = handle_upload_inner(req);
    xSemaphoreGive(s_upload_write_mutex);
    return err;
}

static esp_err_t handle_delete_photo(httpd_req_t *req)
{
    if (web_mode_blocks_album_io()) {
        return send_result(req, false, "上传模式进行中，暂不允许删除照片", -44);
    }
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许删除", -21);
    }
    const char *encoded_name = req->uri + strlen("/api/v1/photos/");
    char path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    if (!decode_uri_component(encoded_name, path, sizeof(path))) {
        return send_result(req, false, "路径解析失败", -1);
    }
    if (gallery_service_find_index_by_path(path) < 0) {
        return send_result(req, false, "未找到要删除的照片", -3);
    }
    esp_err_t err = enqueue_delete_photo_path(path);
    if (err != ESP_OK) {
        return send_result(req, false, "删除失败", err);
    }
    return send_result(req, true, "删除成功", 0);
}

static esp_err_t handle_set_boot_image_from_photo(httpd_req_t *req)
{
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，请稍后再试", -21);
    }

    cJSON *json = NULL;
    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "请求参数错误", -1);
    }

    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(json, "path");
    if (!cJSON_IsString(path_item) || !path_item->valuestring || !path_item->valuestring[0]) {
        cJSON_Delete(json);
        return send_result(req, false, "缺少照片路径", -2);
    }

    ephoto_photo_t photo = {0};
    int index = gallery_service_find_index_by_path(path_item->valuestring);
    if (index < 0 || gallery_service_get_item(index, &photo) != ESP_OK) {
        cJSON_Delete(json);
        return send_result(req, false, "未找到要设置的照片", -3);
    }
    cJSON_Delete(json);

    void *pixels = NULL;
    uint16_t width = 0;
    uint16_t height = 0;
    esp_err_t err = display_service_decode_jpeg_file_to_screen_rgb565(photo.path,
                                                                      EPHOTO_FIT_COVER,
                                                                      0,
                                                                      &pixels,
                                                                      &width,
                                                                      &height);
    if (err != ESP_OK) {
        return send_result(req, false, "生成开机画面失败，请确认这张图片可正常显示", err);
    }

    err = boot_image_service_set_custom_rgb565(photo.name, pixels, width, height);
    free(pixels);
    if (err != ESP_OK) {
        return send_result(req, false, "保存自定义开机画面失败", err);
    }

    return send_result(req, true, "已设为自定义开机画面，下次启动生效", 0);
}

static esp_err_t handle_restore_default_boot_image(httpd_req_t *req)
{
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，请稍后再试", -21);
    }
    esp_err_t err = boot_image_service_use_default();
    if (err != ESP_OK) {
        return send_result(req, false, "恢复默认开机画面失败", err);
    }
    return send_result(req, true, "已恢复默认开机画面，下次启动生效", 0);
}

static esp_err_t handle_factory_reset(httpd_req_t *req)
{
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许恢复出厂设置", -61);
    }
    if (factory_reset_service_is_running()) {
        return send_result(req, false, "恢复出厂设置正在执行，请稍候", -62);
    }

    set_web_mode(EPHOTO_WEB_MODE_CONTROL);
    esp_err_t err = factory_reset_service_run();
    if (err != ESP_OK) {
        return send_result(req, false, "恢复出厂设置失败，请稍后重试", err);
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddStringToObject(json, "message", "恢复出厂设置完成，设备即将自动重启");
    cJSON_AddBoolToObject(json, "rebooting", true);
    cJSON_AddNumberToObject(json, "rebootDelayMs", 1500);
    esp_err_t resp_err = send_json_response(req, json);
    factory_reset_service_schedule_reboot(1500);
    return resp_err;
}

static esp_err_t handle_set_custom_boot_image(httpd_req_t *req)
{
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，请稍后再试", -21);
    }

    char encoded_name[EPHOTO_MAX_ENCODED_NAME_LEN] = {0};
    char filename[EPHOTO_MAX_PHOTO_NAME_LEN] = {0};
    size_t encoded_name_len = httpd_req_get_hdr_value_len(req, "X-Filename-B64");
    if (encoded_name_len > 0 && encoded_name_len < sizeof(encoded_name) &&
        httpd_req_get_hdr_value_str(req, "X-Filename-B64", encoded_name, sizeof(encoded_name)) == ESP_OK) {
        (void)decode_base64_filename(encoded_name, filename, sizeof(filename));
    }
    if (!filename[0]) {
        encoded_name_len = httpd_req_get_hdr_value_len(req, "X-Filename");
        if (encoded_name_len > 0 && encoded_name_len < sizeof(encoded_name) &&
            httpd_req_get_hdr_value_str(req, "X-Filename", encoded_name, sizeof(encoded_name)) == ESP_OK) {
            strlcpy(filename, encoded_name, sizeof(filename));
        }
    }

    if (req->content_len <= 0) {
        return send_result(req, false, "未接收到图片数据", -2);
    }

    uint8_t *jpeg_data = heap_caps_malloc((size_t)req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_data) {
        jpeg_data = malloc((size_t)req->content_len);
    }
    if (!jpeg_data) {
        return send_result(req, false, "内存不足，无法处理开机画面", -10);
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req,
                                      (char *)jpeg_data + offset,
                                      remaining > EPHOTO_UPLOAD_IO_CHUNK ? EPHOTO_UPLOAD_IO_CHUNK : remaining);
        if (received <= 0) {
            free(jpeg_data);
            return send_result(req, false, "上传中断，请重新选择图片", -8);
        }
        remaining -= received;
        offset += received;
    }

    void *pixels = NULL;
    uint16_t width = 0;
    uint16_t height = 0;
    esp_err_t err = display_service_decode_jpeg_rgb565(jpeg_data,
                                                       (size_t)req->content_len,
                                                       1280,
                                                       800,
                                                       &pixels,
                                                       &width,
                                                       &height);
    free(jpeg_data);
    if (err != ESP_OK || !pixels || width != 1280 || height != 800) {
        free(pixels);
        return send_result(req, false, "开机画面格式不正确，请使用系统提供的裁切上传流程", err == ESP_OK ? -18 : err);
    }

    const char *display_name = filename[0] ? filename : "自定义开机画面";
    err = boot_image_service_set_custom_rgb565(display_name, pixels, width, height);
    free(pixels);
    if (err != ESP_OK) {
        return send_result(req, false, "保存自定义开机画面失败", err);
    }

    return send_result(req, true, "自定义开机画面已保存，下次启动生效", 0);
}

static esp_err_t handle_delete_photos(httpd_req_t *req)
{
    if (web_mode_blocks_album_io()) {
        return send_result(req, false, "上传模式进行中，暂不允许删除照片", -44);
    }
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许删除", -21);
    }

    cJSON *json = NULL;
    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "JSON 解析失败", -1);
    }

    cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(json);
        return send_result(req, false, "缺少待删除列表", -2);
    }

    int removed = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, items) {
        if (!cJSON_IsString(entry) || !entry->valuestring) {
            continue;
        }
        if (gallery_service_find_index_by_path(entry->valuestring) >= 0 &&
            enqueue_delete_photo_path(entry->valuestring) == ESP_OK) {
            ++removed;
        }
    }

    cJSON_Delete(json);

    if (removed <= 0) {
        return send_result(req, false, "没有删除任何图片", -3);
    }

    return send_result(req, true, "批量删除成功", removed);
}

static esp_err_t handle_rebuild_photo(httpd_req_t *req)
{
    if (web_mode_blocks_album_io()) {
        return send_result(req, false, "上传模式进行中，暂不允许重建缓存", -45);
    }
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许重建缓存", -26);
    }

    cJSON *json = NULL;
    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "JSON 解析失败", -1);
    }

    int selected_index = -1;
    cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "path");
    if (cJSON_IsString(path) && path->valuestring) {
        selected_index = gallery_service_find_index_by_path(path->valuestring);
    }
    cJSON_Delete(json);

    if (selected_index < 0) {
        return send_result(req, false, "未找到目标图片", -2);
    }

    ephoto_photo_t photo = {0};
    if (gallery_service_get_item(selected_index, &photo) != ESP_OK) {
        return send_result(req, false, "未找到目标图片", -2);
    }

    ephoto_command_t cmd = {
        .type = EPHOTO_CMD_REBUILD_PHOTO_INDEX,
        .value_i32 = selected_index,
    };
    if (!s_command_handler) {
        return send_result(req, false, "控制器未就绪", -27);
    }
    esp_err_t err = s_command_handler(&cmd);
    if (err != ESP_OK) {
        return send_command_busy(req, err, "系统正忙，请稍后重试");
    }
    return send_result(req, true, "已开始重建该照片缓存", 0);
}

static esp_err_t handle_adjust_photo_orientation(httpd_req_t *req)
{
    if (web_mode_blocks_album_io()) {
        return send_result(req, false, "上传模式进行中，暂不允许调整照片方向", -47);
    }
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许调整照片方向", -48);
    }

    cJSON *json = NULL;
    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "JSON 解析失败", -1);
    }

    int selected_index = -1;
    ephoto_command_type_t command_type = EPHOTO_CMD_SET_PHOTO_ROTATION_90_INDEX;
    const char *result_message = "已设置为旋转90度并开始重建缓存";
    cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "path");
    cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
    if (cJSON_IsString(path) && path->valuestring) {
        selected_index = gallery_service_find_index_by_path(path->valuestring);
    }
    if (cJSON_IsString(action) && action->valuestring) {
        if (strcmp(action->valuestring, "rotate_180") == 0) {
            command_type = EPHOTO_CMD_SET_PHOTO_ROTATION_180_INDEX;
            result_message = "已设置为旋转180度并开始重建缓存";
        } else if (strcmp(action->valuestring, "rotate_270") == 0) {
            command_type = EPHOTO_CMD_SET_PHOTO_ROTATION_270_INDEX;
            result_message = "已设置为旋转270度并开始重建缓存";
        } else if (strcmp(action->valuestring, "reset") == 0) {
            command_type = EPHOTO_CMD_RESET_PHOTO_ORIENTATION_INDEX;
            result_message = "已恢复方向并开始重建缓存";
        } else {
            command_type = EPHOTO_CMD_SET_PHOTO_ROTATION_90_INDEX;
            result_message = "已设置为旋转90度并开始重建缓存";
        }
    }
    cJSON_Delete(json);

    if (selected_index < 0) {
        return send_result(req, false, "未找到目标图片", -2);
    }

    ephoto_photo_t photo = {0};
    if (gallery_service_get_item(selected_index, &photo) != ESP_OK) {
        return send_result(req, false, "未找到目标图片", -2);
    }
    if (!photo_supports_orientation_adjustment_web(&photo)) {
        return send_result(req, false, "当前只有 JPEG 照片支持方向修正", -49);
    }

    ephoto_command_t cmd = {
        .type = command_type,
        .value_i32 = selected_index,
    };
    if (!s_command_handler) {
        return send_result(req, false, "控制器未就绪", -27);
    }
    esp_err_t err = s_command_handler(&cmd);
    if (err != ESP_OK) {
        return send_command_busy(req, err, "系统正忙，请稍后重试");
    }
    return send_result(req, true, result_message, 0);
}

static esp_err_t handle_finalize_upload(httpd_req_t *req)
{
    cJSON *json = NULL;
    char latest_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    int uploaded_count = 0;
    char cache_note[EPHOTO_MAX_NOTIFICATION_LEN] = {0};

    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许整理新上传照片", -26);
    }

    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "JSON 解析失败", -1);
    }

    cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "latestPath");
    if (cJSON_IsString(path) && path->valuestring) {
        strlcpy(latest_path, path->valuestring, sizeof(latest_path));
    }

    cJSON *count = cJSON_GetObjectItemCaseSensitive(json, "uploadedCount");
    if (cJSON_IsNumber(count)) {
        uploaded_count = count->valueint;
    }
    cJSON_Delete(json);

    if (uploaded_count > 1) {
        snprintf(cache_note, sizeof(cache_note), "正在为 %d 张新照片生成缓存", uploaded_count);
    } else {
        strlcpy(cache_note, "正在为新上传照片生成缓存", sizeof(cache_note));
    }

    esp_err_t err = enqueue_upload_finalize_command(latest_path, cache_note);
    if (err != ESP_OK) {
        return send_command_busy(req, err, "系统正忙，暂时无法安排新照片缓存任务");
    }
    upload_finalize_watchdog_clear();
    if (s_web_mode == EPHOTO_WEB_MODE_UPLOAD) {
        set_web_mode(EPHOTO_WEB_MODE_CONTROL);
    }
    return send_result(req, true, "已开始整理新上传照片", 0);
}

static esp_err_t handle_select_photo(httpd_req_t *req)
{
    if (web_mode_blocks_album_io()) {
        return send_result(req, false, "上传模式进行中，暂不允许切换图片", -46);
    }
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许切换图片", -23);
    }

    cJSON *json = NULL;
    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "JSON 解析失败", -1);
    }

    int selected_index = -1;
    cJSON *index = cJSON_GetObjectItemCaseSensitive(json, "index");
    cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "path");
    if (cJSON_IsNumber(index)) {
        selected_index = index->valueint;
    } else if (cJSON_IsString(path) && path->valuestring) {
        selected_index = gallery_service_find_index_by_path(path->valuestring);
    }
    cJSON_Delete(json);

    if (selected_index < 0) {
        return send_result(req, false, "未找到目标图片", -2);
    }

    ephoto_app_state_t snapshot = {0};
    ephoto_photo_t photo = {0};
    if (s_snapshot_fn) {
        s_snapshot_fn(&snapshot);
    }
    if (gallery_service_get_item(selected_index, &photo) != ESP_OK) {
        return send_result(req, false, "未找到目标图片", -2);
    }
    if (!photo_matches_orientation_filter_web(&photo, snapshot.settings.orientation_filter)) {
        char message[192];
        build_orientation_block_message(&photo,
                                        snapshot.settings.orientation_filter,
                                        message,
                                        sizeof(message));
        return send_result(req, false, message, -24);
    }

    ephoto_command_t cmd = {
        .type = EPHOTO_CMD_SHOW_PHOTO_INDEX,
        .value_i32 = selected_index,
    };
    if (s_command_handler) {
        s_command_handler(&cmd);
    }
    return send_result(req, true, "已切换到所选图片", 0);
}

static esp_err_t read_json_body(httpd_req_t *req, cJSON **out_json)
{
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    int remaining = req->content_len;
    int received = 0;
    while (remaining > 0) {
        int len = httpd_req_recv(req, body + received, remaining);
        if (len <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += len;
        remaining -= len;
    }
    *out_json = cJSON_Parse(body);
    free(body);
    return *out_json ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t handle_update_settings(httpd_req_t *req)
{
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许修改设置", -22);
    }
    ephoto_app_state_t snapshot = {0};
    if (s_snapshot_fn) {
        s_snapshot_fn(&snapshot);
    }
    cJSON *json = NULL;
    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "JSON 解析失败", -1);
    }

    cJSON *mode = cJSON_GetObjectItemCaseSensitive(json, "playbackMode");
    cJSON *interval = cJSON_GetObjectItemCaseSensitive(json, "slideshowInterval");
    cJSON *fit_mode = cJSON_GetObjectItemCaseSensitive(json, "fitMode");
    cJSON *orientation = cJSON_GetObjectItemCaseSensitive(json, "orientationFilter");
    cJSON *rotation = cJSON_GetObjectItemCaseSensitive(json, "rotationDeg");
    cJSON *auto_rotation_enabled = cJSON_GetObjectItemCaseSensitive(json, "autoRotationEnabled");
    cJSON *brightness = cJSON_GetObjectItemCaseSensitive(json, "brightnessPercent");
    cJSON *brightness_mode = cJSON_GetObjectItemCaseSensitive(json, "brightnessMode");
    cJSON *screen = cJSON_GetObjectItemCaseSensitive(json, "screenOn");
    cJSON *screen_schedule_enabled = cJSON_GetObjectItemCaseSensitive(json, "screenScheduleEnabled");
    cJSON *screen_on_time = cJSON_GetObjectItemCaseSensitive(json, "screenOnTime");
    cJSON *screen_off_time = cJSON_GetObjectItemCaseSensitive(json, "screenOffTime");
    cJSON *clock = cJSON_GetObjectItemCaseSensitive(json, "clockVisible");
    cJSON *clock_format = cJSON_GetObjectItemCaseSensitive(json, "clockFormat");
    cJSON *clock_position = cJSON_GetObjectItemCaseSensitive(json, "clockPosition");
    cJSON *clock_color = cJSON_GetObjectItemCaseSensitive(json, "clockColor");
    cJSON *ota_interval = cJSON_GetObjectItemCaseSensitive(json, "otaInterval");
    cJSON *ota_channel = cJSON_GetObjectItemCaseSensitive(json, "otaChannel");
    cJSON *device_name = cJSON_GetObjectItemCaseSensitive(json, "deviceName");
    cJSON *timezone = cJSON_GetObjectItemCaseSensitive(json, "timezone");
    cJSON *auto_time_sync = cJSON_GetObjectItemCaseSensitive(json, "autoTimeSync");
    cJSON *manual_time = cJSON_GetObjectItemCaseSensitive(json, "manualTime");
    cJSON *sync_now = cJSON_GetObjectItemCaseSensitive(json, "syncNow");

    if (cJSON_IsString(mode)) {
        ephoto_playback_mode_t next_mode = parse_mode(mode->valuestring);
        if (snapshot.settings.playback_mode != next_mode) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_PLAYBACK_MODE, next_mode);
        }
    }
    if (cJSON_IsString(interval)) {
        ephoto_slideshow_interval_t next_interval = parse_interval(interval->valuestring);
        if (snapshot.settings.slideshow_interval != next_interval) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_INTERVAL, next_interval);
        }
    }
    if (cJSON_IsString(fit_mode)) {
        ephoto_fit_mode_t next_fit = parse_fit_mode(fit_mode->valuestring);
        if (snapshot.settings.fit_mode != next_fit) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_FIT_MODE, next_fit);
        }
    }
    if (cJSON_IsString(orientation)) {
        ephoto_orientation_filter_t next_orientation = parse_orientation_filter(orientation->valuestring);
        if (snapshot.settings.orientation_filter != next_orientation) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_ORIENTATION_FILTER, next_orientation);
        }
    }
    if (cJSON_IsNumber(rotation)) {
        int next_rotation = rotation->valueint == 90 ? 90 : 0;
        if ((int)snapshot.settings.manual_rotation_deg != next_rotation) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_ROTATION, next_rotation);
        }
    }
    if (cJSON_IsBool(auto_rotation_enabled)) {
        bool next_auto_rotation_enabled = cJSON_IsTrue(auto_rotation_enabled);
        if (snapshot.settings.auto_rotation_enabled != next_auto_rotation_enabled) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_AUTO_ROTATION, next_auto_rotation_enabled);
        }
    }
    if (cJSON_IsNumber(brightness)) {
        int value = brightness->valueint;
        if (value < 0) value = 0;
        if (value > EPHOTO_BRIGHTNESS_MAX) value = EPHOTO_BRIGHTNESS_MAX;
        if ((int)snapshot.settings.manual_brightness != value ||
            snapshot.settings.brightness_mode != EPHOTO_BRIGHTNESS_MODE_MANUAL) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_BRIGHTNESS, value);
        }
    }
    if (cJSON_IsString(brightness_mode)) {
        ephoto_brightness_mode_t next_brightness_mode = parse_brightness_mode(brightness_mode->valuestring);
        if (snapshot.settings.brightness_mode != next_brightness_mode) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_BRIGHTNESS_MODE, next_brightness_mode);
        }
    }
    if (cJSON_IsBool(screen)) {
        bool next_screen = cJSON_IsTrue(screen);
        if (snapshot.settings.screen_on != next_screen) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_SCREEN_ON, next_screen);
        }
    }
    if (cJSON_IsBool(screen_schedule_enabled)) {
        bool next_schedule_enabled = cJSON_IsTrue(screen_schedule_enabled);
        if (snapshot.settings.screen_schedule_enabled != next_schedule_enabled) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_SCREEN_SCHEDULE_ENABLED, next_schedule_enabled);
        }
    }
    if (cJSON_IsString(screen_on_time) && screen_on_time->valuestring) {
        uint16_t next_on_minute = 0;
        if (parse_hhmm_string(screen_on_time->valuestring, &next_on_minute) &&
            snapshot.settings.screen_on_minute != next_on_minute) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_SCREEN_ON_MINUTE, next_on_minute);
        }
    }
    if (cJSON_IsString(screen_off_time) && screen_off_time->valuestring) {
        uint16_t next_off_minute = 0;
        if (parse_hhmm_string(screen_off_time->valuestring, &next_off_minute) &&
            snapshot.settings.screen_off_minute != next_off_minute) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_SCREEN_OFF_MINUTE, next_off_minute);
        }
    }
    if (cJSON_IsBool(clock)) {
        bool next_clock = cJSON_IsTrue(clock);
        if (snapshot.settings.clock_visible != next_clock) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_CLOCK_VISIBLE, next_clock);
        }
    }
    if (cJSON_IsString(clock_format)) {
        ephoto_clock_format_t next_clock_format = parse_clock_format(clock_format->valuestring);
        if (snapshot.settings.clock_format != next_clock_format) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_CLOCK_FORMAT, next_clock_format);
        }
    }
    if (cJSON_IsString(clock_position)) {
        ephoto_clock_position_t next_clock_position = parse_clock_position(clock_position->valuestring);
        if (snapshot.settings.clock_position != next_clock_position) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_CLOCK_POSITION, next_clock_position);
        }
    }
    if (cJSON_IsString(clock_color)) {
        ephoto_clock_color_t next_clock_color = parse_clock_color(clock_color->valuestring);
        if (snapshot.settings.clock_color != next_clock_color) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_CLOCK_COLOR, next_clock_color);
        }
    }
    if (cJSON_IsString(ota_interval)) {
        ephoto_ota_interval_t next_ota_interval = parse_ota_interval(ota_interval->valuestring);
        if (snapshot.settings.ota_interval != next_ota_interval) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_OTA_INTERVAL, next_ota_interval);
        }
    }
    if (cJSON_IsString(ota_channel) && ota_channel->valuestring) {
        const char *next_ota_channel = "stable";
        if (strcmp(ota_channel->valuestring, "stable") == 0) {
            next_ota_channel = "stable";
        } else if (strcmp(ota_channel->valuestring, "beta") == 0) {
            next_ota_channel = "beta";
        }
        ESP_LOGI(TAG, "settings update request: otaChannel=%s current=%s", next_ota_channel, snapshot.settings.ota_channel);
        if (strcmp(snapshot.settings.ota_channel, next_ota_channel) != 0) {
            ephoto_command_t cmd = {.type = EPHOTO_CMD_SET_OTA_CHANNEL};
            strlcpy(cmd.text, next_ota_channel, sizeof(cmd.text));
            s_command_handler(&cmd);
            strlcpy(snapshot.settings.ota_channel, next_ota_channel, sizeof(snapshot.settings.ota_channel));
        }
    }
    if (cJSON_IsString(device_name) && device_name->valuestring && device_name->valuestring[0]) {
        const char *current_name = wifi_admin_get_device_name();
        if (!current_name || strcmp(current_name, device_name->valuestring) != 0) {
            esp_err_t rename_err = wifi_admin_set_device_name(device_name->valuestring);
            if (rename_err != ESP_OK) {
                cJSON_Delete(json);
                return send_result(req, false, "设备名称保存失败", rename_err);
            }
        }
    }
    if (cJSON_IsString(timezone) && timezone->valuestring && timezone->valuestring[0] &&
        strcmp(snapshot.settings.timezone, timezone->valuestring) != 0) {
        ephoto_command_t cmd = {.type = EPHOTO_CMD_SET_TIMEZONE};
        strlcpy(cmd.text, timezone->valuestring, sizeof(cmd.text));
        s_command_handler(&cmd);
    }
    if (cJSON_IsBool(auto_time_sync)) {
        bool next_auto_time_sync = cJSON_IsTrue(auto_time_sync);
        if (snapshot.settings.auto_time_sync != next_auto_time_sync) {
            (void)enqueue_simple_command(EPHOTO_CMD_SET_AUTO_TIME_SYNC, next_auto_time_sync);
        }
    }
    if (cJSON_IsString(manual_time) && manual_time->valuestring && manual_time->valuestring[0]) {
        ephoto_command_t cmd = {.type = EPHOTO_CMD_SET_MANUAL_TIME};
        strlcpy(cmd.text, manual_time->valuestring, sizeof(cmd.text));
        s_command_handler(&cmd);
    }
    if (cJSON_IsBool(sync_now) && cJSON_IsTrue(sync_now)) {
        (void)enqueue_simple_command(EPHOTO_CMD_SYNC_TIME_NOW, 1);
    }
    cJSON_Delete(json);
    (void)enqueue_notification(EPHOTO_NOTIFICATION_SUCCESS, "设置已保存");
    return send_result(req, true, "设置已保存", 0);
}

static esp_err_t handle_ota_check(httpd_req_t *req)
{
    if (!s_command_handler) {
        return send_result(req, false, "控制器未就绪", -1);
    }
    if (!wifi_admin_is_sta_connected()) {
        return send_result(req, false, "设备未连接家庭网络，无法访问 OTA 服务", ESP_ERR_INVALID_STATE);
    }
    ephoto_command_t cmd = {
        .type = EPHOTO_CMD_OTA_CHECK_NOW,
    };
    if (req->content_len > 0) {
        cJSON *json = NULL;
        if (read_json_body(req, &json) != ESP_OK) {
            return send_result(req, false, "JSON 解析失败", -1);
        }
        cJSON *channel = cJSON_GetObjectItemCaseSensitive(json, "channel");
        if (cJSON_IsString(channel) && channel->valuestring) {
            if (strcmp(channel->valuestring, "beta") == 0) {
                strlcpy(cmd.text, "beta", sizeof(cmd.text));
            } else {
                strlcpy(cmd.text, "stable", sizeof(cmd.text));
            }
        } else {
            strlcpy(cmd.text, "stable", sizeof(cmd.text));
        }
        cJSON_Delete(json);
    } else {
        strlcpy(cmd.text, "stable", sizeof(cmd.text));
    }
    esp_err_t err = s_command_handler(&cmd);
    if (err != ESP_OK) {
        return send_result(req, false, "无法开始检查更新，请稍后重试", err);
    }
    return send_result(req, true, "已开始检查更新", 0);
}

static esp_err_t handle_ota_update(httpd_req_t *req)
{
    if (!s_command_handler) {
        return send_result(req, false, "控制器未就绪", -1);
    }
    ephoto_command_t cmd = {
        .type = EPHOTO_CMD_OTA_UPDATE_NOW,
    };
    esp_err_t err = s_command_handler(&cmd);
    if (err != ESP_OK) {
        return send_result(req, false, "无法开始安装更新", err);
    }
    return send_result(req, true, "已开始下载并安装更新", 0);
}

static esp_err_t handle_wifi_saved(httpd_req_t *req)
{
    ephoto_wifi_profile_t items[EPHOTO_MAX_WIFI_PROFILES];
    size_t count = wifi_admin_get_profiles(items, EPHOTO_MAX_WIFI_PROFILES);
    cJSON *json = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(json, "items");
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", items[i].ssid);
        cJSON_AddItemToArray(array, item);
    }
    return send_json_response(req, json);
}

static const char *wifi_auth_mode_to_string(uint8_t auth_mode)
{
    switch (auth_mode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa_wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2_wpa3";
    default:
        return "unknown";
    }
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    if (web_mode_blocks_album_io()) {
        return send_result(req, false, "上传模式进行中，暂不允许扫描 Wi-Fi", -43);
    }
    (void)enqueue_notification(EPHOTO_NOTIFICATION_INFO, "正在扫描附近 Wi-Fi...");
    ephoto_wifi_scan_result_t items[16] = {0};
    size_t count = 0;
    esp_err_t err = wifi_admin_scan(items, 16, &count);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        (void)enqueue_notification(EPHOTO_NOTIFICATION_ERROR, "当前未连上家庭 Wi-Fi，无法扫描附近网络");
        return send_result(req,
                           false,
                           "设备当前处于配网热点模式，请手动填写 SSID，或先让设备连上家庭 Wi-Fi 后再扫描",
                           -30);
    }
    if (err != ESP_OK) {
        (void)enqueue_notification(EPHOTO_NOTIFICATION_ERROR, "Wi-Fi 扫描失败");
        return send_result(req, false, "扫描 Wi-Fi 失败", err);
    }

    {
        char note[EPHOTO_MAX_NOTIFICATION_LEN];
        snprintf(note, sizeof(note), "Wi-Fi 扫描完成，找到 %u 个热点", (unsigned)count);
        (void)enqueue_notification(EPHOTO_NOTIFICATION_SUCCESS, note);
    }

    cJSON *json = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(json, "items");
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", items[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", items[i].rssi);
        cJSON_AddStringToObject(item, "authMode", wifi_auth_mode_to_string(items[i].auth_mode));
        cJSON_AddItemToArray(array, item);
    }
    return send_json_response(req, json);
}

static esp_err_t handle_wifi_connect(httpd_req_t *req)
{
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许 Wi-Fi 操作", -23);
    }
    cJSON *json = NULL;
    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "JSON 解析失败", -1);
    }
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!cJSON_IsString(ssid)) {
        cJSON_Delete(json);
        return send_result(req, false, "SSID 不能为空", -2);
    }

    if (!s_command_handler) {
        cJSON_Delete(json);
        return send_result(req, false, "控制器未就绪", -1);
    }
    ephoto_command_t cmd = {.type = EPHOTO_CMD_CONNECT_WIFI};
    strlcpy(cmd.ssid, ssid->valuestring, sizeof(cmd.ssid));
    if (cJSON_IsString(password)) {
        strlcpy(cmd.password, password->valuestring, sizeof(cmd.password));
    }
    cJSON_Delete(json);
    esp_err_t err = s_command_handler(&cmd);
    if (err != ESP_OK) {
        return send_result(req, false, "无法开始切换 Wi-Fi", err);
    }
    {
        char note[EPHOTO_MAX_NOTIFICATION_LEN];
        snprintf(note, sizeof(note), "正在切换到 Wi-Fi: %s", cmd.ssid);
        (void)enqueue_notification(EPHOTO_NOTIFICATION_INFO, note);
    }
    return send_result(req, true, "Wi-Fi 切换已开始", 0);
}

static esp_err_t handle_wifi_delete(httpd_req_t *req)
{
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，暂不允许 Wi-Fi 操作", -24);
    }
    const char *encoded_ssid = req->uri + strlen("/api/v1/wifi/");
    char ssid[EPHOTO_MAX_SSID_LEN] = {0};
    if (!decode_uri_component(encoded_ssid, ssid, sizeof(ssid)) || !ssid[0]) {
        return send_result(req, false, "SSID 不能为空", -1);
    }
    esp_err_t err = wifi_admin_remove(ssid);
    if (err == ESP_OK) {
        (void)enqueue_simple_command(EPHOTO_CMD_REFRESH_WIFI_STATE, 0);
        {
            char note[EPHOTO_MAX_NOTIFICATION_LEN];
            snprintf(note, sizeof(note), "Wi-Fi 已删除: %s", ssid);
            (void)enqueue_notification(EPHOTO_NOTIFICATION_SUCCESS, note);
        }
        return send_result(req, true, "Wi-Fi 已删除", 0);
    }
    return send_result(req, false, "删除失败", err);
}

static esp_err_t handle_action(httpd_req_t *req)
{
    if (interaction_locked_now()) {
        return send_result(req, false, "启动缓存处理中，请等待全部图片缓存完成", -25);
    }
    const char *action = req->uri + strlen("/api/v1/actions/");
    if (web_mode_blocks_heavy_actions(action)) {
        return send_result(req, false, "上传模式进行中，暂不允许执行该操作", -47);
    }
    if (strcmp(action, "rotate") == 0 && auto_rotation_enabled_now()) {
        return send_result(req, false, "已开启自动旋转，请先关闭自动旋转后再手动旋转", -50);
    }
    esp_err_t err = ESP_OK;
    if (strcmp(action, "hw-confirm") == 0) err = enqueue_simple_command(EPHOTO_CMD_HW_CONFIRM, 0);
    else if (strcmp(action, "hw-prev") == 0) err = enqueue_simple_command(EPHOTO_CMD_HW_PREV, 0);
    else if (strcmp(action, "hw-next") == 0) err = enqueue_simple_command(EPHOTO_CMD_HW_NEXT, 0);
    else if (strcmp(action, "hw-rotate") == 0) err = enqueue_simple_command(EPHOTO_CMD_HW_ROTATE, 0);
    else if (strcmp(action, "hw-zoom") == 0) err = enqueue_simple_command(EPHOTO_CMD_HW_ZOOM, 0);
    else if (strcmp(action, "prev") == 0) err = enqueue_simple_command(EPHOTO_CMD_PREV, 0);
    else if (strcmp(action, "next") == 0) err = enqueue_simple_command(EPHOTO_CMD_NEXT, 0);
    else if (strcmp(action, "confirm") == 0) err = enqueue_simple_command(EPHOTO_CMD_CONFIRM, 0);
    else if (strcmp(action, "menu") == 0) err = enqueue_simple_command(EPHOTO_CMD_MENU_LONGPRESS, 0);
    else if (strcmp(action, "rotate") == 0) err = enqueue_simple_command(EPHOTO_CMD_ROTATE, 0);
    else if (strcmp(action, "fit") == 0) err = enqueue_simple_command(EPHOTO_CMD_FIT_TOGGLE, 0);
    else if (strcmp(action, "screen") == 0) err = enqueue_simple_command(EPHOTO_CMD_SCREEN_TOGGLE, 0);
    else if (strcmp(action, "brightness") == 0) err = enqueue_simple_command(EPHOTO_CMD_BRIGHTNESS_STEP, 0);
    else if (strcmp(action, "clock") == 0) err = enqueue_simple_command(EPHOTO_CMD_CLOCK_TOGGLE, 0);
    else if (strcmp(action, "wifi-ap") == 0) {
        (void)enqueue_notification(EPHOTO_NOTIFICATION_INFO, "正在切换到配网热点模式...");
        err = enqueue_simple_command(EPHOTO_CMD_START_WIFI_AP, 0);
    }
    else if (strcmp(action, "cache-purge") == 0) err = enqueue_simple_command(EPHOTO_CMD_PURGE_CACHE, 0);
    else if (strcmp(action, "build-cache") == 0) err = enqueue_simple_command(EPHOTO_CMD_BUILD_MISSING_CACHE, 0);
    else if (strcmp(action, "reprocess-current") == 0) err = enqueue_simple_command(EPHOTO_CMD_REPROCESS_CURRENT, 0);
    else if (strcmp(action, "rescan-media") == 0) err = enqueue_simple_command(EPHOTO_CMD_RESCAN_MEDIA, 0);
    else return send_result(req, false, "未知动作", -1);
    if (err != ESP_OK) {
        return send_command_busy(req, err, "系统正忙，请稍后重试");
    }
    return send_result(req, true, "操作成功", 0);
}

static esp_err_t handle_web_mode(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_body(req, &json) != ESP_OK) {
        return send_result(req, false, "JSON 解析失败", -1);
    }

    cJSON *mode = cJSON_GetObjectItemCaseSensitive(json, "mode");
    if (!cJSON_IsString(mode) || !mode->valuestring) {
        cJSON_Delete(json);
        return send_result(req, false, "缺少模式参数", -2);
    }

    set_web_mode(parse_web_mode_string(mode->valuestring));
    cJSON_Delete(json);
    return send_result(req, true, "模式已切换", 0);
}

esp_err_t web_api_start(web_state_snapshot_fn_t snapshot_fn, input_command_handler_t handler)
{
    s_snapshot_fn = snapshot_fn;
    s_command_handler = handler;
    set_web_mode(EPHOTO_WEB_MODE_CONTROL);

    s_upload_finalize_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_upload_finalize_mutex, ESP_ERR_NO_MEM, TAG, "create upload recovery mutex failed");
    s_upload_write_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_upload_write_mutex, ESP_ERR_NO_MEM, TAG, "create upload write mutex failed");
    const esp_timer_create_args_t upload_finalize_timer_args = {
        .callback = upload_finalize_watchdog_callback,
        .name = "upload_finalize",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&upload_finalize_timer_args, &s_upload_finalize_timer),
                        TAG,
                        "create upload recovery timer failed");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 32;
    // Upload handling has a deliberately large multipart parser frame in
    // addition to the HTTP server call chain. Keep enough headroom for the
    // parser and response serialization when a client uploads via FormData.
    config.stack_size = 24576;
    config.task_priority = tskIDLE_PRIORITY + 9;
    config.core_id = 0;
    config.max_req_hdr_len = 2048;
    config.max_uri_len = 2048;
    // Keep several of the 16 LwIP sockets free for ESP-Hosted and Wi-Fi.
    // The socket limit and LRU purge also bound idle keep-alive connections.
    config.max_open_sockets = EPHOTO_HTTP_MAX_OPEN_SOCKETS;
    config.backlog_conn = 6;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 45;
    config.send_wait_timeout = 45;
    // Keep connections alive for mobile clients.  The mini-program treats a
    // server-side close after every upload response as a device disconnect.
    // The socket limit and LRU purge above still bound idle connection use.
    config.keep_alive_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "http server start failed");

    httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_root},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = handle_favicon},
        {.uri = "/style.css*", .method = HTTP_GET, .handler = handle_css},
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = handle_status},
        {.uri = "/api/v1/storage", .method = HTTP_GET, .handler = handle_storage},
        {.uri = "/api/v1/storage/benchmark", .method = HTTP_GET, .handler = handle_storage_benchmark},
        {.uri = "/api/v1/photos", .method = HTTP_GET, .handler = handle_photos},
        {.uri = "/api/v1/thumb/*", .method = HTTP_GET, .handler = handle_thumb},
        {.uri = "/api/v1/photos/upload", .method = HTTP_POST, .handler = handle_upload},
        {.uri = "/api/v1/photos/finalize-upload", .method = HTTP_POST, .handler = handle_finalize_upload},
        {.uri = "/api/v1/photos/select", .method = HTTP_POST, .handler = handle_select_photo},
        {.uri = "/api/v1/photos/rebuild", .method = HTTP_POST, .handler = handle_rebuild_photo},
        {.uri = "/api/v1/photos/orientation", .method = HTTP_POST, .handler = handle_adjust_photo_orientation},
        {.uri = "/api/v1/photos/delete", .method = HTTP_POST, .handler = handle_delete_photos},
        {.uri = "/api/v1/photos/*", .method = HTTP_DELETE, .handler = handle_delete_photo},
        {.uri = "/api/v1/boot-image/select", .method = HTTP_POST, .handler = handle_set_boot_image_from_photo},
        {.uri = "/api/v1/boot-image/custom", .method = HTTP_POST, .handler = handle_set_custom_boot_image},
        {.uri = "/api/v1/boot-image/default", .method = HTTP_POST, .handler = handle_restore_default_boot_image},
        {.uri = "/api/v1/system/factory-reset", .method = HTTP_POST, .handler = handle_factory_reset},
        {.uri = "/api/v1/settings", .method = HTTP_PUT, .handler = handle_update_settings},
        {.uri = "/api/v1/ota/check", .method = HTTP_POST, .handler = handle_ota_check},
        {.uri = "/api/v1/ota/update", .method = HTTP_POST, .handler = handle_ota_update},
        {.uri = "/api/v1/web/mode", .method = HTTP_PUT, .handler = handle_web_mode},
        {.uri = "/api/v1/wifi/saved", .method = HTTP_GET, .handler = handle_wifi_saved},
        {.uri = "/api/v1/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect},
        {.uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan},
        {.uri = "/api/v1/wifi/*", .method = HTTP_DELETE, .handler = handle_wifi_delete},
        {.uri = "/api/v1/actions/*", .method = HTTP_POST, .handler = handle_action},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "route register failed");
    }
    ESP_LOGI(TAG, "web api started");
    return ESP_OK;
}

bool web_api_is_running(void)
{
    return s_server != NULL;
}
