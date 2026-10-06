#include "ota_service.h"

#include <string.h>

#ifndef EPHOTO_ENABLE_OTA
#define EPHOTO_ENABLE_OTA 0
#endif

#if EPHOTO_ENABLE_OTA

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "clock_service.h"
#include "wifi_admin.h"

static const char *TAG = "ota_service";
static const char *OTA_NAMESPACE = "ota";
static const char *OTA_PRODUCT_KEY = EPHOTO_OTA_PRODUCT_KEY;
static const char *OTA_DEVICE_TYPE = EPHOTO_OTA_DEVICE_TYPE_KEY;
static const char *OTA_DEFAULT_CHANNEL = EPHOTO_OTA_CHANNEL_KEY;
static const char *OTA_PROVIDER = "custom-ota-api";

#define OTA_QUEUE_LEN          4
#define OTA_TASK_STACK         16384
#define OTA_TASK_PRIO          5
#define OTA_RESPONSE_BODY_MAX  16384
#define OTA_HTTP_TIMEOUT_MS    15000
#define OTA_HEARTBEAT_INTERVAL_MS (12LL * 60 * 60 * 1000)
#define OTA_HEARTBEAT_RETRY_DELAY_MS (30 * 1000)
#define OTA_HEARTBEAT_MAX_RETRIES 6

#ifndef EPHOTO_OTA_SERVER_URL
#define EPHOTO_OTA_SERVER_URL ""
#endif

#ifndef EPHOTO_OTA_PRODUCT_KEY
#define EPHOTO_OTA_PRODUCT_KEY "ephoto-p4"
#endif

#ifndef EPHOTO_OTA_DEVICE_TYPE_KEY
#define EPHOTO_OTA_DEVICE_TYPE_KEY "ephoto-esp32p4"
#endif

#ifndef EPHOTO_OTA_CHANNEL_KEY
#define EPHOTO_OTA_CHANNEL_KEY "stable"
#endif

static const char *resolved_ota_channel(const ephoto_settings_t *settings)
{
    if (settings && settings->ota_channel[0]) {
        if (strcmp(settings->ota_channel, "stable") == 0 ||
            strcmp(settings->ota_channel, "beta") == 0) {
            return settings->ota_channel;
        }
    }
    return OTA_DEFAULT_CHANNEL;
}

static void log_ota_json_request(const char *label, const char *url, const char *payload)
{
    ESP_LOGI(TAG, "%s: url=%s payload=%s", label, url ? url : "", payload ? payload : "{}");
}

typedef enum {
    OTA_REQ_CHECK = 0,
    OTA_REQ_HEARTBEAT,
    OTA_REQ_UPDATE,
} ota_request_type_t;

typedef struct {
    ota_request_type_t type;
    bool manual;
    ephoto_settings_t settings;
} ota_request_t;

typedef struct {
    char version[EPHOTO_MAX_OTA_VERSION_LEN];
    char title[EPHOTO_MAX_OTA_ARTIFACT_LEN];
    char release_id[EPHOTO_MAX_OTA_DEPLOYMENT_LEN];
    char channel[EPHOTO_MAX_OTA_CHANNEL_LEN];
    char download_url[EPHOTO_MAX_OTA_URL_LEN];
    char release_notes[EPHOTO_MAX_OTA_NOTES_LEN];
    char sha256[65];
    size_t size_bytes;
    bool force_update;
    bool has_update;
} ota_release_info_t;

static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static TaskHandle_t s_task_handle;
static ephoto_ota_status_t s_status;
static bool s_initialized;
static int64_t s_last_auto_request_ms;
static int64_t s_last_heartbeat_ms;
static int64_t s_last_heartbeat_enqueue_ms;
static int64_t s_last_heartbeat_fail_ms;
static uint8_t s_heartbeat_retry_count;
static char s_device_uid[EPHOTO_MAX_OTA_DEPLOYMENT_LEN];
static char s_selected_offer_channel[EPHOTO_MAX_OTA_CHANNEL_LEN];
static ota_release_info_t s_cached_offer;
static bool s_pending_boot_report;
static bool s_pending_boot_success;
static bool s_boot_confirmation_required;
static char s_pending_boot_from_version[EPHOTO_MAX_OTA_VERSION_LEN];
static char s_pending_boot_version[EPHOTO_MAX_OTA_VERSION_LEN];
static char s_pending_boot_release_id[EPHOTO_MAX_OTA_DEPLOYMENT_LEN];
static bool s_runtime_busy;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t wall_clock_ms(void)
{
    return clock_service_wall_time_ms();
}

static const char *ota_server_url(void)
{
    return EPHOTO_OTA_SERVER_URL;
}

static const char *map_server_message_cn(const char *message)
{
    if (!message || !message[0]) {
        return NULL;
    }
    if (strcmp(message, "already_latest") == 0) {
        return "当前已是最新版本";
    }
    if (strcmp(message, "no_release_published") == 0) {
        return "当前通道还没有已发布固件";
    }
    if (strcmp(message, "release_not_published") == 0) {
        return "目标固件还未发布";
    }
    if (strcmp(message, "release_binary_missing") == 0) {
        return "当前型号缺少对应固件文件";
    }
    if (strcmp(message, "channel_not_found") == 0) {
        return "更新通道不存在或未启用";
    }
    if (strcmp(message, "device_type_not_found") == 0) {
        return "设备型号不存在或未启用";
    }
    if (strcmp(message, "product_not_found") == 0) {
        return "产品不存在或未启用";
    }
    if (strcmp(message, "device_not_found") == 0) {
        return "设备未在 OTA 平台登记";
    }
    return message;
}

static const char *map_ota_error_cn(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return NULL;
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_TIMEOUT:
        return "无法连接 OTA 服务器，请检查网络或服务器地址";
    case ESP_ERR_INVALID_RESPONSE:
        return "OTA 服务器返回了无效数据";
    case ESP_ERR_NO_MEM:
        return "设备内存不足，无法检查更新";
    case ESP_ERR_INVALID_STATE:
        return "设备未连接家庭网络，无法访问 OTA 服务";
    default:
        return "检查更新失败";
    }
}

static const char *current_version_string(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc && desc->version[0]) {
        return desc->version;
    }
#ifdef EPHOTO_PROJECT_VER
    return EPHOTO_PROJECT_VER;
#else
    return "unknown";
#endif
}

static void format_version_for_ota(const char *version, char *out_value, size_t out_len)
{
    if (!out_value || out_len == 0) {
        return;
    }

    out_value[0] = '\0';
    if (!version || !version[0]) {
        strlcpy(out_value, "v0.0.0", out_len);
        return;
    }

    if (version[0] == 'v' || version[0] == 'V') {
        strlcpy(out_value, version, out_len);
        return;
    }

    snprintf(out_value, out_len, "v%s", version);
}

static void ensure_device_uid(void)
{
    if (s_device_uid[0]) {
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_device_uid,
             sizeof(s_device_uid),
             "%s-%02x%02x%02x%02x%02x%02x",
             OTA_PRODUCT_KEY,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void status_set_message(ephoto_ota_status_t *status, const char *message)
{
    if (!status) {
        return;
    }
    strlcpy(status->message, message ? message : "", sizeof(status->message));
}

static void status_reset_available_locked(void)
{
    s_status.update_available = false;
    s_status.available_version[0] = '\0';
    s_status.available_artifact[0] = '\0';
    s_status.deployment_id[0] = '\0';
    s_status.release_notes[0] = '\0';
    s_selected_offer_channel[0] = '\0';
    memset(&s_cached_offer, 0, sizeof(s_cached_offer));
}

static void status_init_locked(void)
{
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_cached_offer, 0, sizeof(s_cached_offer));
    strlcpy(s_status.provider, OTA_PROVIDER, sizeof(s_status.provider));
    strlcpy(s_status.device_type, OTA_DEVICE_TYPE, sizeof(s_status.device_type));
    strlcpy(s_status.channel, OTA_DEFAULT_CHANNEL, sizeof(s_status.channel));
    strlcpy(s_status.current_version, current_version_string(), sizeof(s_status.current_version));
    s_status.stage = EPHOTO_OTA_STAGE_IDLE;
    status_set_message(&s_status,
                       ota_server_url()[0] ? "等待检查更新" : "未编译 OTA 服务地址");
}

static void queue_boot_report_locked(bool success,
                                     const char *from_version,
                                     const char *to_version,
                                     const char *release_id)
{
    s_pending_boot_report = true;
    s_pending_boot_success = success;
    strlcpy(s_pending_boot_from_version, from_version ? from_version : "", sizeof(s_pending_boot_from_version));
    strlcpy(s_pending_boot_version, to_version ? to_version : "", sizeof(s_pending_boot_version));
    strlcpy(s_pending_boot_release_id, release_id ? release_id : "", sizeof(s_pending_boot_release_id));
}

static void clear_pending_update(nvs_handle_t handle)
{
    (void)nvs_erase_key(handle, "pending");
    (void)nvs_erase_key(handle, "pending_from");
    (void)nvs_erase_key(handle, "pending_ver");
    (void)nvs_erase_key(handle, "pending_rel");
    (void)nvs_commit(handle);
}

static void record_pending_update_result_locked(bool boot_confirmed,
                                                const char *pending_from_version,
                                                const char *pending_version,
                                                const char *pending_release_id)
{
    if (boot_confirmed) {
        s_status.stage = EPHOTO_OTA_STAGE_SUCCESS;
        s_status.last_error = ESP_OK;
        s_status.last_update_ms = wall_clock_ms();
        s_status.last_check_ok = true;
        s_status.reboot_required = false;
        if (pending_version && pending_version[0]) {
            strlcpy(s_status.current_version, pending_version, sizeof(s_status.current_version));
            snprintf(s_status.message, sizeof(s_status.message), "固件已更新到 %s", pending_version);
        } else {
            status_set_message(&s_status, "固件更新成功");
        }
    } else {
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.last_error = ESP_FAIL;
        s_status.last_update_ms = wall_clock_ms();
        s_status.last_check_ok = false;
        if (pending_version && pending_version[0]) {
            snprintf(s_status.message,
                     sizeof(s_status.message),
                     "更新到 %s 未通过启动确认，系统已回滚",
                     pending_version);
        } else {
            status_set_message(&s_status, "新固件未通过启动确认，系统已回滚");
        }
    }
    queue_boot_report_locked(boot_confirmed,
                             pending_from_version,
                             pending_version,
                             pending_release_id);
}

static void load_pending_update_result_locked(void)
{
    nvs_handle_t handle = 0;
    char pending_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    char pending_from_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    char pending_release_id[EPHOTO_MAX_OTA_DEPLOYMENT_LEN] = {0};
    char running_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    size_t pending_len = sizeof(pending_version);
    size_t pending_from_len = sizeof(pending_from_version);
    size_t release_id_len = sizeof(pending_release_id);
    uint8_t pending = 0;

    if (nvs_open(OTA_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    if (nvs_get_u8(handle, "pending", &pending) != ESP_OK || pending == 0) {
        nvs_close(handle);
        return;
    }

    (void)nvs_get_str(handle, "pending_ver", pending_version, &pending_len);
    (void)nvs_get_str(handle, "pending_from", pending_from_version, &pending_from_len);
    (void)nvs_get_str(handle, "pending_rel", pending_release_id, &release_id_len);

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    bool have_state = running &&
                      esp_ota_get_state_partition(running, &ota_state) == ESP_OK;
    bool waiting_for_confirmation = false;
    bool boot_confirmed = false;

    format_version_for_ota(current_version_string(), running_version, sizeof(running_version));

    if (pending_version[0] && strcmp(running_version, pending_version) == 0 &&
        have_state &&
        (ota_state == ESP_OTA_IMG_PENDING_VERIFY || ota_state == ESP_OTA_IMG_NEW)) {
        // Keep the image pending until main confirms that hosted Wi-Fi and all
        // externally visible services survived their startup validation window.
        waiting_for_confirmation = true;
        s_boot_confirmation_required = true;
        s_status.reboot_required = false;
        s_status.last_error = ESP_OK;
        status_set_message(&s_status, "新固件启动验证中");
    } else if (pending_version[0] &&
               strcmp(running_version, pending_version) == 0 &&
               (!have_state ||
                ota_state == ESP_OTA_IMG_VALID ||
                ota_state == ESP_OTA_IMG_UNDEFINED)) {
        // This also recovers a power loss after confirmation but before NVS
        // cleanup, and remains compatible with devices carrying an old
        // bootloader until they receive a full-image migration.
        boot_confirmed = true;
    }

    if (!waiting_for_confirmation) {
        record_pending_update_result_locked(boot_confirmed,
                                            pending_from_version,
                                            pending_version,
                                            pending_release_id);
        clear_pending_update(handle);
    }
    nvs_close(handle);
}

static void save_pending_update(const char *from_version, const char *to_version, const char *release_id)
{
    nvs_handle_t handle = 0;
    if (nvs_open(OTA_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    (void)nvs_set_u8(handle, "pending", 1);
    (void)nvs_set_str(handle, "pending_from", from_version ? from_version : "");
    (void)nvs_set_str(handle, "pending_ver", to_version ? to_version : "");
    (void)nvs_set_str(handle, "pending_rel", release_id ? release_id : "");
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static bool ota_server_configured(void)
{
    return ota_server_url()[0] != '\0';
}

static void apply_settings_to_status_locked(const ephoto_settings_t *settings)
{
    char version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    if (!settings) {
        return;
    }
    s_status.configured = ota_server_configured();
    strlcpy(s_status.provider, OTA_PROVIDER, sizeof(s_status.provider));
    strlcpy(s_status.channel, resolved_ota_channel(settings), sizeof(s_status.channel));
    format_version_for_ota(current_version_string(), version, sizeof(version));
    strlcpy(s_status.current_version, version, sizeof(s_status.current_version));
    if (!s_status.configured && s_status.stage == EPHOTO_OTA_STAGE_IDLE) {
        status_set_message(&s_status, "未编译 OTA 服务地址");
    }
}

static int parse_next_version_component(const char **cursor, bool *ok)
{
    const char *p = cursor ? *cursor : NULL;
    while (p && *p && !isdigit((unsigned char)*p)) {
        ++p;
    }
    if (!p || !*p) {
        *ok = false;
        if (cursor) {
            *cursor = p;
        }
        return 0;
    }

    int value = 0;
    while (*p && isdigit((unsigned char)*p)) {
        value = value * 10 + (*p - '0');
        ++p;
    }
    *ok = true;
    if (cursor) {
        *cursor = p;
    }
    return value;
}

static int compare_version_strings(const char *lhs, const char *rhs)
{
    if (!lhs && !rhs) {
        return 0;
    }
    if (!lhs) {
        return -1;
    }
    if (!rhs) {
        return 1;
    }
    if (strcmp(lhs, rhs) == 0) {
        return 0;
    }

    const char *pl = lhs;
    const char *pr = rhs;
    bool any_numeric = false;
    while (true) {
        bool ok_l = false;
        bool ok_r = false;
        int vl = parse_next_version_component(&pl, &ok_l);
        int vr = parse_next_version_component(&pr, &ok_r);
        if (!ok_l && !ok_r) {
            break;
        }
        any_numeric = true;
        if (!ok_l) {
            return -1;
        }
        if (!ok_r) {
            return 1;
        }
        if (vl != vr) {
            return vl > vr ? 1 : -1;
        }
    }

    return any_numeric ? 0 : strcmp(lhs, rhs);
}

static void build_ota_endpoint(const char *base,
                               const char *path,
                               char *out_url,
                               size_t out_url_len)
{
    size_t base_len = 0;
    bool base_has_slash = false;
    bool path_has_slash = false;

    if (!out_url || out_url_len == 0) {
        return;
    }
    out_url[0] = '\0';

    if (!base || !base[0] || !path || !path[0]) {
        return;
    }

    base_len = strlen(base);
    base_has_slash = base_len > 0 && base[base_len - 1] == '/';
    path_has_slash = path[0] == '/';

    if (base_has_slash && path_has_slash) {
        snprintf(out_url, out_url_len, "%.*s%s", (int)(base_len - 1), base, path);
    } else if (!base_has_slash && !path_has_slash) {
        snprintf(out_url, out_url_len, "%s/%s", base, path);
    } else {
        snprintf(out_url, out_url_len, "%s%s", base, path);
    }
}

static esp_err_t http_json_request(const char *url,
                                   esp_http_client_method_t method,
                                   const char *json_body,
                                   char **out_body,
                                   int *out_status_code)
{
    if (!url || !out_body || !out_status_code) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;
    *out_status_code = -1;

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "E-Photo OTA",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    if (json_body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }

    esp_err_t err = esp_http_client_open(client, json_body ? (int)strlen(json_body) : 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    if (json_body) {
        int written = esp_http_client_write(client, json_body, (int)strlen(json_body));
        if (written < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    *out_status_code = status_code;

    size_t cap = (content_len > 0 && content_len < OTA_RESPONSE_BODY_MAX)
                     ? (size_t)content_len + 1U
                     : OTA_RESPONSE_BODY_MAX;
    char *body = calloc(1, cap);
    if (!body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total + 1U < cap) {
        int read_len = esp_http_client_read(client, body + total, (int)(cap - total - 1U));
        if (read_len < 0) {
            free(body);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (read_len == 0) {
            break;
        }
        total += (size_t)read_len;
    }
    body[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    *out_body = body;
    return ESP_OK;
}

static esp_err_t build_identity_payload(const ephoto_settings_t *settings, char **out_json)
{
    cJSON *root = NULL;
    char *rendered = NULL;
    char firmware_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};

    if (!settings || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }

    ensure_device_uid();

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "deviceUid", s_device_uid);
    cJSON_AddStringToObject(root, "deviceName", wifi_admin_get_device_name());
    cJSON_AddStringToObject(root, "productKey", OTA_PRODUCT_KEY);
    cJSON_AddStringToObject(root, "deviceTypeKey", OTA_DEVICE_TYPE);
    cJSON_AddStringToObject(root, "channelKey", resolved_ota_channel(settings));
    format_version_for_ota(current_version_string(), firmware_version, sizeof(firmware_version));
    cJSON_AddStringToObject(root, "firmwareVersion", firmware_version);
    cJSON_AddStringToObject(root, "ipHint", wifi_admin_get_sta_ip());

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = rendered;
    return ESP_OK;
}

static esp_err_t build_report_payload(const ephoto_settings_t *settings,
                                      const char *release_id,
                                      const char *event_name,
                                      const char *from_version,
                                      const char *to_version,
                                      const char *message,
                                      const char *reason,
                                      char **out_json)
{
    cJSON *root = NULL;
    cJSON *payload = NULL;
    char *rendered = NULL;
    char from_version_norm[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    char to_version_norm[EPHOTO_MAX_OTA_VERSION_LEN] = {0};

    if (!settings || !event_name || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }

    ensure_device_uid();

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "deviceUid", s_device_uid);
    cJSON_AddStringToObject(root, "deviceName", wifi_admin_get_device_name());
    cJSON_AddStringToObject(root, "productKey", OTA_PRODUCT_KEY);
    cJSON_AddStringToObject(root, "deviceTypeKey", OTA_DEVICE_TYPE);
    cJSON_AddStringToObject(root, "channelKey", resolved_ota_channel(settings));
    if (release_id && release_id[0]) {
        char *end = NULL;
        long release_id_num = strtol(release_id, &end, 10);
        if (end && *end == '\0') {
            cJSON_AddNumberToObject(root, "releaseId", release_id_num);
        } else {
            cJSON_AddStringToObject(root, "releaseId", release_id);
        }
    }
    cJSON_AddStringToObject(root, "event", event_name);
    format_version_for_ota(from_version, from_version_norm, sizeof(from_version_norm));
    format_version_for_ota(to_version, to_version_norm, sizeof(to_version_norm));
    cJSON_AddStringToObject(root, "fromVersion", from_version && from_version[0] ? from_version_norm : "");
    cJSON_AddStringToObject(root, "toVersion", to_version && to_version[0] ? to_version_norm : "");
    cJSON_AddStringToObject(root, "message", message ? message : "");
    if (reason && reason[0]) {
        payload = cJSON_AddObjectToObject(root, "payload");
        if (payload) {
            cJSON_AddStringToObject(payload, "reason", reason);
        }
    }

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = rendered;
    return ESP_OK;
}

static esp_err_t parse_check_response(const char *body, ota_release_info_t *out_info, char *server_message, size_t server_message_len)
{
    cJSON *root = NULL;
    cJSON *file = NULL;
    cJSON *policy = NULL;
    cJSON *node = NULL;

    if (!body || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_info, 0, sizeof(*out_info));
    if (server_message && server_message_len > 0) {
        server_message[0] = '\0';
    }

    root = cJSON_Parse(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    node = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (node && cJSON_IsBool(node) && !cJSON_IsTrue(node)) {
        node = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (server_message && server_message_len > 0 && cJSON_IsString(node)) {
            strlcpy(server_message, node->valuestring, server_message_len);
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    node = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (server_message && server_message_len > 0 && cJSON_IsString(node)) {
        strlcpy(server_message, node->valuestring, server_message_len);
    }

    node = cJSON_GetObjectItemCaseSensitive(root, "hasUpdate");
    out_info->has_update = cJSON_IsTrue(node);
    if (!out_info->has_update) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    node = cJSON_GetObjectItemCaseSensitive(root, "releaseId");
    if (cJSON_IsString(node)) {
        strlcpy(out_info->release_id, node->valuestring, sizeof(out_info->release_id));
    } else if (cJSON_IsNumber(node)) {
        snprintf(out_info->release_id, sizeof(out_info->release_id), "%.0f", node->valuedouble);
    }

    node = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsString(node)) {
        strlcpy(out_info->version, node->valuestring, sizeof(out_info->version));
    }

    node = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (cJSON_IsString(node)) {
        strlcpy(out_info->title, node->valuestring, sizeof(out_info->title));
    }

    node = cJSON_GetObjectItemCaseSensitive(root, "releaseNotes");
    if (cJSON_IsString(node)) {
        strlcpy(out_info->release_notes, node->valuestring, sizeof(out_info->release_notes));
    }

    file = cJSON_GetObjectItemCaseSensitive(root, "file");
    if (!cJSON_IsObject(file)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    node = cJSON_GetObjectItemCaseSensitive(file, "downloadUrl");
    if (cJSON_IsString(node)) {
        strlcpy(out_info->download_url, node->valuestring, sizeof(out_info->download_url));
    }
    node = cJSON_GetObjectItemCaseSensitive(file, "sha256");
    if (cJSON_IsString(node)) {
        strlcpy(out_info->sha256, node->valuestring, sizeof(out_info->sha256));
    }
    node = cJSON_GetObjectItemCaseSensitive(file, "sizeBytes");
    if (cJSON_IsNumber(node) && node->valuedouble > 0) {
        out_info->size_bytes = (size_t)node->valuedouble;
    }
    node = cJSON_GetObjectItemCaseSensitive(file, "name");
    if (cJSON_IsString(node) && !out_info->title[0]) {
        strlcpy(out_info->title, node->valuestring, sizeof(out_info->title));
    }

    policy = cJSON_GetObjectItemCaseSensitive(root, "policy");
    if (cJSON_IsObject(policy)) {
        node = cJSON_GetObjectItemCaseSensitive(policy, "force");
        out_info->force_update = cJSON_IsTrue(node);
    }

    cJSON_Delete(root);
    if (!out_info->version[0] || !out_info->download_url[0]) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t send_report_event(const ephoto_settings_t *settings,
                                   const char *release_id,
                                   const char *event_name,
                                   const char *from_version,
                                   const char *to_version,
                                   const char *message,
                                   const char *reason)
{
    char url[EPHOTO_MAX_OTA_URL_LEN + 64];
    char *payload = NULL;
    char *response = NULL;
    int status_code = -1;
    esp_err_t err;

    if (!ota_server_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

    build_ota_endpoint(ota_server_url(), "/api/v1/device/report", url, sizeof(url));
    if (!url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_report_payload(settings, release_id, event_name, from_version, to_version, message, reason, &payload);
    if (err != ESP_OK) {
        return err;
    }

    log_ota_json_request("OTA report request", url, payload);
    err = http_json_request(url, HTTP_METHOD_POST, payload, &response, &status_code);
    free(payload);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA report %s failed: %s", event_name, esp_err_to_name(err));
        free(response);
        return err;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG,
                 "OTA report %s rejected: http %d body=%s",
                 event_name,
                 status_code,
                 response ? response : "");
        free(response);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA report %s response: status=%d body=%s", event_name, status_code, response ? response : "");
    free(response);
    return ESP_OK;
}

static esp_err_t send_heartbeat(const ephoto_settings_t *settings)
{
    char url[EPHOTO_MAX_OTA_URL_LEN + 64];
    char *payload = NULL;
    char *response = NULL;
    int status_code = -1;
    esp_err_t err;

    if (!ota_server_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

    build_ota_endpoint(ota_server_url(), "/api/v1/device/heartbeat", url, sizeof(url));
    if (!url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_identity_payload(settings, &payload);
    if (err != ESP_OK) {
        return err;
    }

    log_ota_json_request("OTA heartbeat request", url, payload);
    err = http_json_request(url, HTTP_METHOD_POST, payload, &response, &status_code);
    free(payload);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA heartbeat failed: %s", esp_err_to_name(err));
        s_last_heartbeat_fail_ms = now_ms();
        if (s_heartbeat_retry_count < UINT8_MAX) {
            s_heartbeat_retry_count += 1U;
        }
        free(response);
        return err;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "OTA heartbeat rejected: http %d body=%s", status_code, response ? response : "");
        s_last_heartbeat_fail_ms = now_ms();
        if (s_heartbeat_retry_count < UINT8_MAX) {
            s_heartbeat_retry_count += 1U;
        }
        free(response);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA heartbeat response: status=%d body=%s", status_code, response ? response : "");
    free(response);
    s_last_heartbeat_ms = now_ms();
    s_last_heartbeat_enqueue_ms = s_last_heartbeat_ms;
    s_last_heartbeat_fail_ms = 0;
    s_heartbeat_retry_count = 0;
    return ESP_OK;
}

static esp_err_t flush_pending_boot_report(const ephoto_settings_t *settings)
{
    esp_err_t err;
    const char *event_name;
    const char *message;

    if (!s_pending_boot_report || !settings || !ota_server_configured()) {
        return ESP_OK;
    }

    event_name = s_pending_boot_success ? "boot_confirmed" : "install_failed";
    message = s_pending_boot_success ? "new firmware boot ok" : "new firmware rolled back";
    err = send_report_event(settings,
                            s_pending_boot_release_id,
                            event_name,
                            s_pending_boot_from_version,
                            s_pending_boot_version,
                            message,
                            s_pending_boot_success ? NULL : "boot_rollback");
    if (err == ESP_OK) {
        s_pending_boot_report = false;
        s_pending_boot_success = false;
        s_pending_boot_from_version[0] = '\0';
        s_pending_boot_version[0] = '\0';
        s_pending_boot_release_id[0] = '\0';
    }
    return err;
}

static esp_err_t fetch_update_offer(const ephoto_settings_t *settings,
                                    ota_release_info_t *out_info,
                                    int *out_status_code,
                                    char *server_message,
                                    size_t server_message_len)
{
    char url[EPHOTO_MAX_OTA_URL_LEN + 64];
    char *payload = NULL;
    char *body = NULL;
    int status_code = -1;
    esp_err_t err;

    if (!settings || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }

    build_ota_endpoint(ota_server_url(), "/api/v1/device/check-update", url, sizeof(url));
    if (!url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_identity_payload(settings, &payload);
    if (err != ESP_OK) {
        return err;
    }

    log_ota_json_request("OTA check request", url, payload);

    err = http_json_request(url, HTTP_METHOD_POST, payload, &body, &status_code);
    free(payload);
    if (out_status_code) {
        *out_status_code = status_code;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA check transport failed: %s", esp_err_to_name(err));
        free(body);
        return err;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "OTA check http rejected: status=%d body=%s",
                 status_code,
                 body ? body : "");
        free(body);
        return ESP_FAIL;
    }

    err = parse_check_response(body, out_info, server_message, server_message_len);
    ESP_LOGI(TAG, "OTA check response: status=%d body=%s", status_code, body ? body : "");
    free(body);
    return err;
}

static void sha256_bytes_to_hex(const uint8_t *hash, char *out_hex, size_t out_hex_len)
{
    static const char *HEX = "0123456789abcdef";
    if (!hash || !out_hex || out_hex_len < 65) {
        if (out_hex && out_hex_len > 0) {
            out_hex[0] = '\0';
        }
        return;
    }

    for (size_t i = 0; i < 32; ++i) {
        out_hex[i * 2] = HEX[(hash[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = HEX[hash[i] & 0x0F];
    }
    out_hex[64] = '\0';
}

static bool starts_with_ignore_case(const char *value, const char *prefix)
{
    if (!value || !prefix) {
        return false;
    }
    while (*prefix) {
        if (tolower((unsigned char)*value) != tolower((unsigned char)*prefix)) {
            return false;
        }
        ++value;
        ++prefix;
    }
    return true;
}

static esp_err_t perform_heartbeat(const ephoto_settings_t *settings)
{
    return send_heartbeat(settings);
}

static void update_status_after_check_locked(const ephoto_settings_t *settings,
                                             bool manual_trigger,
                                             const ota_release_info_t *info,
                                             esp_err_t err,
                                             const char *server_message)
{
    s_status.last_check_ms = wall_clock_ms();
    s_status.last_manual_trigger = manual_trigger;
    s_status.checking = false;
    s_status.updating = false;
    s_status.progress_percent = 0;
    apply_settings_to_status_locked(settings);

    if (err != ESP_OK) {
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.last_error = err;
        s_status.last_check_ok = false;
        status_reset_available_locked();
        status_set_message(&s_status,
                           (server_message && server_message[0]) ? server_message : map_ota_error_cn(err));
        return;
    }

    s_status.last_error = ESP_OK;
    s_status.last_check_ok = true;

    if (!info || !info->has_update) {
        s_status.stage = EPHOTO_OTA_STAGE_UP_TO_DATE;
        status_reset_available_locked();
        status_set_message(&s_status,
                           (server_message && server_message[0])
                               ? map_server_message_cn(server_message)
                               : "当前已是最新版本");
        return;
    }

    if (compare_version_strings(info->version, s_status.current_version) <= 0) {
        s_status.stage = EPHOTO_OTA_STAGE_UP_TO_DATE;
        status_reset_available_locked();
        status_set_message(&s_status, "当前已是最新版本");
        return;
    }

    s_status.stage = EPHOTO_OTA_STAGE_UPDATE_AVAILABLE;
    s_status.update_available = true;
    strlcpy(s_selected_offer_channel, resolved_ota_channel(settings), sizeof(s_selected_offer_channel));
    strlcpy(s_status.available_version, info->version, sizeof(s_status.available_version));
    strlcpy(s_status.available_artifact, info->title, sizeof(s_status.available_artifact));
    strlcpy(s_status.deployment_id, info->release_id, sizeof(s_status.deployment_id));
    strlcpy(s_status.release_notes, info->release_notes, sizeof(s_status.release_notes));
    s_cached_offer = *info;
    snprintf(s_status.message,
             sizeof(s_status.message),
             "发现新版本 %.*s",
             EPHOTO_MAX_OTA_VERSION_LEN - 1,
             info->version);
}

static esp_err_t perform_check(const ephoto_settings_t *settings, bool manual_trigger)
{
    char server_message[EPHOTO_MAX_OTA_MESSAGE_LEN] = {0};
    ota_release_info_t info = {0};
    int status_code = -1;
    esp_err_t err;

    if (!ota_server_configured()) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        apply_settings_to_status_locked(settings);
        s_status.stage = EPHOTO_OTA_STAGE_IDLE;
        s_status.checking = false;
        s_status.last_error = ESP_ERR_INVALID_STATE;
        s_status.last_check_ok = false;
        s_status.last_manual_trigger = manual_trigger;
        status_set_message(&s_status, "固件未编译 OTA 服务地址");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    apply_settings_to_status_locked(settings);
    s_status.stage = EPHOTO_OTA_STAGE_CHECKING;
    s_status.checking = true;
    s_status.updating = false;
    s_status.last_manual_trigger = manual_trigger;
    s_status.progress_percent = 0;
    status_set_message(&s_status, "正在检查更新");
    xSemaphoreGive(s_mutex);

    err = fetch_update_offer(settings, &info, &status_code, server_message, sizeof(server_message));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    update_status_after_check_locked(settings, manual_trigger, err == ESP_OK ? &info : NULL, err, server_message);
    xSemaphoreGive(s_mutex);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA check completed: http=%d has_update=%d version=%s",
                 status_code, info.has_update, info.version);
    } else {
        ESP_LOGW(TAG, "OTA check failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t perform_update(const ephoto_settings_t *settings, bool manual_trigger)
{
    char target_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    char current_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    char release_id[EPHOTO_MAX_OTA_DEPLOYMENT_LEN] = {0};
    char download_url[EPHOTO_MAX_OTA_URL_LEN] = {0};
    char request_channel[EPHOTO_MAX_OTA_CHANNEL_LEN] = {0};
    ota_release_info_t info = {0};
    char server_message[EPHOTO_MAX_OTA_MESSAGE_LEN] = {0};
    int status_code = -1;
    esp_err_t err;
    ephoto_settings_t request_settings = {0};
    bool need_refresh_offer = false;

    (void)manual_trigger;
    format_version_for_ota(current_version_string(), current_version, sizeof(current_version));
    request_settings = *settings;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_selected_offer_channel[0]) {
        strlcpy(request_channel, s_selected_offer_channel, sizeof(request_channel));
    }
    if (request_channel[0]) {
        strlcpy(request_settings.ota_channel, request_channel, sizeof(request_settings.ota_channel));
    }
    apply_settings_to_status_locked(&request_settings);
    if (!s_status.update_available || !s_status.available_version[0]) {
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.last_error = ESP_ERR_INVALID_STATE;
        status_set_message(&s_status, "当前没有可安装的新版本");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(target_version, s_status.available_version, sizeof(target_version));
    strlcpy(release_id, s_status.deployment_id, sizeof(release_id));
    if (s_cached_offer.has_update &&
        s_cached_offer.download_url[0] &&
        strcmp(s_cached_offer.version, target_version) == 0 &&
        (!request_channel[0] || strcmp(request_channel, s_selected_offer_channel) == 0)) {
        info = s_cached_offer;
    } else {
        need_refresh_offer = true;
    }
    xSemaphoreGive(s_mutex);

    if (need_refresh_offer) {
        ESP_LOGI(TAG, "OTA update: refreshing offer before install");
        err = fetch_update_offer(&request_settings, &info, &status_code, server_message, sizeof(server_message));
        if (err != ESP_OK || !info.has_update || !info.download_url[0]) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.stage = EPHOTO_OTA_STAGE_ERROR;
            s_status.last_error = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
            status_set_message(&s_status,
                               server_message[0] ? server_message : "未获取到有效的固件下载地址");
            xSemaphoreGive(s_mutex);
            (void)send_report_event(&request_settings,
                                    release_id,
                                    "install_failed",
                                    current_version,
                                    target_version,
                                    server_message[0] ? server_message : "missing firmware download url",
                                    "missing_download_url");
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
    } else {
        ESP_LOGI(TAG, "OTA update: reusing cached offer for version %s", target_version);
    }

    if (info.release_id[0]) {
        strlcpy(release_id, info.release_id, sizeof(release_id));
    }
    if (info.version[0]) {
        strlcpy(target_version, info.version, sizeof(target_version));
    }
    strlcpy(download_url, info.download_url, sizeof(download_url));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.stage = EPHOTO_OTA_STAGE_DOWNLOADING;
    s_status.updating = true;
    s_status.checking = false;
    s_status.last_error = ESP_OK;
    s_status.progress_percent = 0;
    status_set_message(&s_status, "正在下载并安装更新");
    xSemaphoreGive(s_mutex);

    esp_http_client_config_t http_cfg = {
        .url = download_url,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = "E-Photo OTA",
    };
    if (starts_with_ignore_case(download_url, "https://")) {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = esp_http_client_open(client, 0);
    }
    if (err != ESP_OK) {
        if (client) {
            esp_http_client_cleanup(client);
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = err;
        status_set_message(&s_status, "无法连接固件下载地址");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(&request_settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "unable to open download stream",
                                "download_open_failed");
        return err;
    }

    int image_size = (int)esp_http_client_fetch_headers(client);
    status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = ESP_FAIL;
        status_set_message(&s_status, "固件下载请求被拒绝");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(&request_settings,
                                release_id,
                                "download_failed",
                                current_version,
                                target_version,
                                "download request rejected",
                                "download_http_rejected");
        return ESP_FAIL;
    }

    size_t expected_size = info.size_bytes > 0 ? info.size_bytes : (image_size > 0 ? (size_t)image_size : 0U);
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = ESP_ERR_NOT_FOUND;
        status_set_message(&s_status, "未找到可用 OTA 分区");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(&request_settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "ota partition not found",
                                "ota_partition_not_found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, expected_size > 0 ? expected_size : OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = err;
        status_set_message(&s_status, "无法开始写入 OTA 分区");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(&request_settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "esp_ota_begin failed",
                                "ota_begin_failed");
        return err;
    }

    uint8_t *buffer = malloc(4096);
    if (!buffer) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = ESP_ERR_NO_MEM;
        status_set_message(&s_status, "分配 OTA 缓冲区失败");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(&request_settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "ota buffer alloc failed",
                                "buffer_alloc_failed");
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    size_t total_read = 0;
    bool download_complete = false;
    bool ota_active = true;

    while (true) {
        int read_len = esp_http_client_read(client, (char *)buffer, 4096);
        if (read_len < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            download_complete = true;
            break;
        }
        mbedtls_sha256_update(&sha_ctx, buffer, (size_t)read_len);
        err = esp_ota_write(ota_handle, buffer, (size_t)read_len);
        if (err != ESP_OK) {
            break;
        }
        total_read += (size_t)read_len;

        uint8_t progress = 0;
        if (expected_size > 0) {
            progress = (uint8_t)((total_read * 100U) / expected_size);
            if (progress > 99) {
                progress = 99;
            }
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.progress_percent = progress;
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!download_complete || err != ESP_OK) {
        if (ota_active) {
            esp_ota_abort(ota_handle);
            ota_active = false;
        }
        free(buffer);
        mbedtls_sha256_free(&sha_ctx);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = err == ESP_OK ? ESP_FAIL : err;
        status_set_message(&s_status, "固件下载未完成");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "firmware data incomplete",
                                "download_incomplete");
        return err == ESP_OK ? ESP_FAIL : err;
    }

    uint8_t sha_bytes[32] = {0};
    char sha_hex[65] = {0};
    mbedtls_sha256_finish(&sha_ctx, sha_bytes);
    mbedtls_sha256_free(&sha_ctx);
    free(buffer);
    sha256_bytes_to_hex(sha_bytes, sha_hex, sizeof(sha_hex));
    if (info.sha256[0] && strcasecmp(sha_hex, info.sha256) != 0) {
        if (ota_active) {
            esp_ota_abort(ota_handle);
            ota_active = false;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = ESP_ERR_INVALID_CRC;
        status_set_message(&s_status, "固件校验失败");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "sha256 mismatch",
                                "checksum_failed");
        return ESP_ERR_INVALID_CRC;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.stage = EPHOTO_OTA_STAGE_APPLYING;
    s_status.progress_percent = 100;
    status_set_message(&s_status, "正在写入新固件");
    xSemaphoreGive(s_mutex);

    err = esp_ota_end(ota_handle);
    ota_active = false;
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = err;
        status_set_message(&s_status, "固件写入失败");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "esp_ota_end failed",
                                "ota_end_failed");
        return err;
    }

    esp_app_desc_t new_desc = {0};
    err = esp_ota_get_partition_description(update_partition, &new_desc);
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = err;
        status_set_message(&s_status, "无法读取新固件信息");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(&request_settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "unable to read firmware image info",
                                "image_info_read_failed");
        return err;
    }

    if (target_version[0] && new_desc.version[0]) {
        char new_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
        format_version_for_ota(new_desc.version, new_version, sizeof(new_version));
        if (strcmp(target_version, new_version) != 0) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.stage = EPHOTO_OTA_STAGE_ERROR;
            s_status.updating = false;
            s_status.last_error = ESP_ERR_INVALID_RESPONSE;
            status_set_message(&s_status, "版本描述与下载固件不一致");
            xSemaphoreGive(s_mutex);
            (void)send_report_event(&request_settings,
                                    release_id,
                                    "install_failed",
                                    current_version,
                                    target_version,
                                    "firmware version mismatch",
                                    "version_mismatch");
            return ESP_ERR_INVALID_RESPONSE;
        }
        strlcpy(target_version, new_version, sizeof(target_version));
    }

    const char *installed_version = target_version[0] ? target_version : new_desc.version;
    // Persist this before changing otadata. If power is lost after the boot
    // target changes, the next image still knows it must validate itself.
    save_pending_update(current_version, installed_version, release_id);

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        nvs_handle_t handle = 0;
        if (nvs_open(OTA_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            clear_pending_update(handle);
            nvs_close(handle);
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.updating = false;
        s_status.last_error = err;
        status_set_message(&s_status, "无法设置新固件为启动分区");
        xSemaphoreGive(s_mutex);
        (void)send_report_event(&request_settings,
                                release_id,
                                "install_failed",
                                current_version,
                                target_version,
                                "set boot partition failed",
                                "set_boot_partition_failed");
        return err;
    }

    (void)send_report_event(&request_settings,
                            release_id,
                            "install_succeeded",
                            current_version,
                            installed_version,
                            "ota finished, rebooting",
                            NULL);

    for (int countdown = 5; countdown >= 1; --countdown) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.stage = EPHOTO_OTA_STAGE_RESTARTING;
        s_status.updating = false;
        s_status.reboot_required = true;
        s_status.last_update_ms = wall_clock_ms();
        s_status.progress_percent = 100;
        s_status.last_error = ESP_OK;
        snprintf(s_status.message,
                 sizeof(s_status.message),
                 "设备即将更新，重启过程中请勿断开电源\n%d秒后开始重启",
                 countdown);
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_restart();
    return ESP_OK;
}

static void ota_task(void *arg)
{
    (void)arg;
    ota_request_t request = {0};
    while (xQueueReceive(s_queue, &request, portMAX_DELAY) == pdTRUE) {
        if (request.type == OTA_REQ_HEARTBEAT) {
            (void)perform_heartbeat(&request.settings);
        } else if (request.type == OTA_REQ_CHECK) {
            (void)perform_check(&request.settings, request.manual);
        } else if (request.type == OTA_REQ_UPDATE) {
            (void)perform_update(&request.settings, request.manual);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t ota_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ensure_device_uid();
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(OTA_QUEUE_LEN, sizeof(ota_request_t));
    if (!s_mutex || !s_queue) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    status_init_locked();
    load_pending_update_result_locked();
    xSemaphoreGive(s_mutex);

    if (xTaskCreatePinnedToCore(ota_task, "ota_task", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, &s_task_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t ota_service_confirm_boot(void)
{
    if (!s_initialized || !s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_boot_confirmation_required) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = running ? esp_ota_get_state_partition(running, &ota_state) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK &&
        (ota_state == ESP_OTA_IMG_PENDING_VERIFY || ota_state == ESP_OTA_IMG_NEW)) {
        err = esp_ota_mark_app_valid_cancel_rollback();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA boot confirmation failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    nvs_handle_t handle = 0;
    char pending_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    char pending_from_version[EPHOTO_MAX_OTA_VERSION_LEN] = {0};
    char pending_release_id[EPHOTO_MAX_OTA_DEPLOYMENT_LEN] = {0};
    size_t pending_len = sizeof(pending_version);
    size_t pending_from_len = sizeof(pending_from_version);
    size_t release_id_len = sizeof(pending_release_id);
    if (nvs_open(OTA_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_get_str(handle, "pending_ver", pending_version, &pending_len);
        (void)nvs_get_str(handle, "pending_from", pending_from_version, &pending_from_len);
        (void)nvs_get_str(handle, "pending_rel", pending_release_id, &release_id_len);
        clear_pending_update(handle);
        nvs_close(handle);
    }

    record_pending_update_result_locked(true,
                                        pending_from_version,
                                        pending_version,
                                        pending_release_id);
    s_boot_confirmation_required = false;
    ESP_LOGI(TAG, "OTA boot verification passed; running image is now valid");
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void ota_service_reject_boot_and_reboot(void)
{
    if (!s_initialized || !s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool reject = s_boot_confirmation_required;
    xSemaphoreGive(s_mutex);
    if (!reject) {
        return;
    }

    ESP_LOGE(TAG, "OTA boot validation failed; rolling back the pending image");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "OTA rollback request failed: %s", esp_err_to_name(err));
}

void ota_service_get_status(ephoto_ota_status_t *out_status)
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

static esp_err_t enqueue_request(ota_request_type_t type,
                                 bool manual_trigger,
                                 const ephoto_settings_t *settings,
                                 bool sta_connected)
{
    if (!s_initialized || !settings) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sta_connected) {
        if (type == OTA_REQ_HEARTBEAT) {
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        apply_settings_to_status_locked(settings);
        s_status.stage = EPHOTO_OTA_STAGE_ERROR;
        s_status.last_error = ESP_ERR_INVALID_STATE;
        s_status.last_check_ok = false;
        status_set_message(&s_status, "设备未连接家庭网络，无法访问 OTA 服务");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ota_request_t req = {
        .type = type,
        .manual = manual_trigger,
        .settings = *settings,
    };

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    apply_settings_to_status_locked(settings);
    if (s_status.checking || s_status.updating) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_mutex);

    if (xQueueSend(s_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (type == OTA_REQ_HEARTBEAT) {
        s_last_heartbeat_enqueue_ms = now_ms();
    }
    return ESP_OK;
}

esp_err_t ota_service_request_check(bool manual_trigger,
                                    const ephoto_settings_t *settings,
                                    bool sta_connected)
{
    return enqueue_request(OTA_REQ_CHECK, manual_trigger, settings, sta_connected);
}

esp_err_t ota_service_request_update(bool manual_trigger,
                                     const ephoto_settings_t *settings,
                                     bool sta_connected)
{
    return enqueue_request(OTA_REQ_UPDATE, manual_trigger, settings, sta_connected);
}

void ota_service_set_runtime_busy(bool busy)
{
    s_runtime_busy = busy;
}

void ota_service_tick(const ephoto_settings_t *settings, bool sta_connected)
{
    if (!s_initialized || !settings) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    apply_settings_to_status_locked(settings);
    bool busy = s_status.checking || s_status.updating;
    int64_t last_check_ms = s_status.last_check_ms;
    xSemaphoreGive(s_mutex);

    if (sta_connected && ota_server_configured() && !busy && s_pending_boot_report) {
        (void)flush_pending_boot_report(settings);
    }

    int64_t now = now_ms();
    bool heartbeat_due = false;

    if (sta_connected && ota_server_configured() && !busy && !s_runtime_busy) {
        bool heartbeat_interval_elapsed = (s_last_heartbeat_enqueue_ms == 0) ||
                                          ((uint64_t)(now - s_last_heartbeat_enqueue_ms) >= OTA_HEARTBEAT_INTERVAL_MS);
        if (heartbeat_interval_elapsed) {
            heartbeat_due = true;
            if (s_heartbeat_retry_count >= OTA_HEARTBEAT_MAX_RETRIES) {
                s_heartbeat_retry_count = 0;
            }
        } else if (s_heartbeat_retry_count > 0 &&
                   s_heartbeat_retry_count < OTA_HEARTBEAT_MAX_RETRIES &&
                   s_last_heartbeat_fail_ms > 0 &&
                   (uint64_t)(now - s_last_heartbeat_fail_ms) >= OTA_HEARTBEAT_RETRY_DELAY_MS &&
                   (uint64_t)(now - s_last_heartbeat_enqueue_ms) >= OTA_HEARTBEAT_RETRY_DELAY_MS) {
            heartbeat_due = true;
        }
    }

    if (heartbeat_due) {
        (void)enqueue_request(OTA_REQ_HEARTBEAT, false, settings, true);
    }

    if (!sta_connected ||
        !ota_server_configured() ||
        s_runtime_busy ||
        settings->ota_interval == EPHOTO_OTA_INTERVAL_OFF ||
        busy) {
        return;
    }

    uint64_t interval_ms = ephoto_ota_interval_to_ms(settings->ota_interval);
    if (interval_ms == 0 ||
        (last_check_ms > 0 && (uint64_t)(now - last_check_ms) < interval_ms) ||
        (s_last_auto_request_ms > 0 && (uint64_t)(now - s_last_auto_request_ms) < interval_ms)) {
        return;
    }

    if (enqueue_request(OTA_REQ_CHECK, false, settings, true) == ESP_OK) {
        s_last_auto_request_ms = now;
    }
}

#else /* EPHOTO_ENABLE_OTA: original OTA implementation is retained above but disabled in the public build. */

esp_err_t ota_service_init(void)
{
    return ESP_OK;
}

esp_err_t ota_service_confirm_boot(void)
{
    return ESP_OK;
}

void ota_service_reject_boot_and_reboot(void)
{
    /* OTA rollback is disabled in the public build. */
}

void ota_service_get_status(ephoto_ota_status_t *out_status)
{
    if (out_status) {
        memset(out_status, 0, sizeof(*out_status));
        out_status->stage = EPHOTO_OTA_STAGE_IDLE;
    }
}

esp_err_t ota_service_request_check(bool manual_trigger,
                                    const ephoto_settings_t *settings,
                                    bool sta_connected)
{
    (void)manual_trigger;
    (void)settings;
    (void)sta_connected;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ota_service_request_update(bool manual_trigger,
                                     const ephoto_settings_t *settings,
                                     bool sta_connected)
{
    (void)manual_trigger;
    (void)settings;
    (void)sta_connected;
    return ESP_ERR_NOT_SUPPORTED;
}

void ota_service_tick(const ephoto_settings_t *settings, bool sta_connected)
{
    (void)settings;
    (void)sta_connected;
}

void ota_service_set_runtime_busy(bool busy)
{
    (void)busy;
}

#endif /* EPHOTO_ENABLE_OTA */
