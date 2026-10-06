#include "ble_provisioning.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include "hosted_service.h"
#include "web_api.h"
#include "wifi_admin.h"

static const char *TAG = "ble_provision";

#define BLEPROV_PROTO_VER            1
#define BLEPROV_PACKET_PAYLOAD_MAX   16
#define BLEPROV_MAX_MESSAGE_SIZE     1536
#define BLEPROV_MAX_REQUEST_ID_LEN   32
#define BLEPROV_MAX_RESULTS          16
#define BLEPROV_QUEUE_LEN            4
#define BLEPROV_CONNECT_TIMEOUT_MS   45000
#define BLEPROV_HTTP_READY_TIMEOUT_MS 5000

static uint8_t s_own_addr_type;
static uint16_t s_info_handle;
static uint16_t s_notify_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_started;
static bool s_synced;
static bool s_should_advertise;
static bool s_advertising;
static bool s_provisioning_busy;
static uint8_t s_next_notify_msg_id;
static char s_ble_name[24];

typedef enum {
    BLEPROV_CMD_HELLO = 0,
    BLEPROV_CMD_WIFI_SCAN,
    BLEPROV_CMD_PROVISION,
} bleprov_cmd_type_t;

typedef struct {
    bleprov_cmd_type_t type;
    char request_id[BLEPROV_MAX_REQUEST_ID_LEN];
    char ssid[EPHOTO_MAX_SSID_LEN];
    char password[EPHOTO_MAX_PASSWORD_LEN];
    char device_name[EPHOTO_MAX_DEVICE_NAME_LEN];
} bleprov_cmd_t;

typedef struct {
    bool active;
    uint8_t msg_id;
    int64_t started_ms;
    size_t len;
    char buffer[BLEPROV_MAX_MESSAGE_SIZE];
} bleprov_rx_state_t;

static QueueHandle_t s_cmd_queue;
static bleprov_rx_state_t s_rx_state;

static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x01, 0x10, 0x00, 0x4f, 0x9d, 0x5a, 0x11, 0x9e,
                     0x7f, 0x4c, 0x2b, 0x6d, 0x01, 0x00, 0x3f, 0x7a);
static const ble_uuid128_t s_info_uuid =
    BLE_UUID128_INIT(0x01, 0x10, 0x00, 0x4f, 0x9d, 0x5a, 0x11, 0x9e,
                     0x7f, 0x4c, 0x2b, 0x6d, 0x02, 0x00, 0x3f, 0x7a);
static const ble_uuid128_t s_write_uuid =
    BLE_UUID128_INIT(0x01, 0x10, 0x00, 0x4f, 0x9d, 0x5a, 0x11, 0x9e,
                     0x7f, 0x4c, 0x2b, 0x6d, 0x03, 0x00, 0x3f, 0x7a);
static const ble_uuid128_t s_notify_uuid =
    BLE_UUID128_INIT(0x01, 0x10, 0x00, 0x4f, 0x9d, 0x5a, 0x11, 0x9e,
                     0x7f, 0x4c, 0x2b, 0x6d, 0x04, 0x00, 0x3f, 0x7a);

static int bleprov_gatt_access(uint16_t conn_handle,
                               uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt,
                               void *arg);
static int bleprov_gap_event(struct ble_gap_event *event, void *arg);
static void bleprov_host_task(void *param);
static void bleprov_on_sync(void);
static void bleprov_on_reset(int reason);
static void bleprov_worker_task(void *arg);
void ble_store_config_init(void);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_info_uuid.u,
                .access_cb = bleprov_gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_info_handle,
            },
            {
                .uuid = &s_write_uuid.u,
                .access_cb = bleprov_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_notify_uuid.u,
                .access_cb = bleprov_gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_notify_handle,
            },
            {0},
        },
    },
    {0},
};

static void bleprov_refresh_state_locked(void);

static int64_t bleprov_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *bleprov_provision_state(void)
{
    if (s_provisioning_busy) {
        return "connecting";
    }
    if (wifi_admin_is_sta_connected()) {
        const char *ip = wifi_admin_get_sta_ip();
        if (ip && ip[0] != '\0') {
            return "online";
        }
    }
    return wifi_admin_has_saved_profiles() ? "provisioned" : "unprovisioned";
}

static const char *bleprov_auth_mode_string(uint8_t auth_mode)
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

static const char *bleprov_reason_code(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "WIFI_AUTH_FAILED";
    case WIFI_REASON_NO_AP_FOUND:
        return "WIFI_NOT_FOUND";
    default:
        return "WIFI_CONNECT_TIMEOUT";
    }
}

static const char *bleprov_reason_message(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:
        return "target wifi not found";
    default:
        return "wifi connect timeout";
    }
}

static void bleprov_log_json_payload(const char *label, cJSON *root)
{
    if (!label || !root) {
        return;
    }

    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        ESP_LOGI(TAG, "%s: <json alloc failed>", label);
        return;
    }

    ESP_LOGI(TAG, "%s: %s", label, text);
    cJSON_free(text);
}

static bool bleprov_should_advertise(void)
{
    if (s_provisioning_busy) {
        return true;
    }

    if (wifi_admin_is_sta_connected()) {
        const char *ip = wifi_admin_get_sta_ip();
        if (ip && ip[0] != '\0') {
            return false;
        }
    }

    return true;
}

static void bleprov_update_ble_name(void)
{
    const char *suffix = wifi_admin_get_device_suffix();
    snprintf(s_ble_name, sizeof(s_ble_name), "E-Photo-%s", suffix && suffix[0] ? suffix : "DEVICE");
}

static cJSON *bleprov_build_info_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    char mdns_url[64] = {0};
    (void)wifi_admin_get_mdns_url(mdns_url, sizeof(mdns_url));

    cJSON_AddStringToObject(root, "product", "e-photo");
    cJSON_AddNumberToObject(root, "protoVer", BLEPROV_PROTO_VER);
    cJSON_AddStringToObject(root, "deviceId", wifi_admin_get_device_id());
    cJSON_AddStringToObject(root, "deviceName", wifi_admin_get_device_name());
    cJSON_AddStringToObject(root, "bleName", s_ble_name);
    cJSON_AddStringToObject(root, "fwVersion", EPHOTO_PROJECT_VER);
    cJSON_AddStringToObject(root, "hwModel", EPHOTO_HW_MODEL);
    cJSON_AddStringToObject(root, "provisionState", bleprov_provision_state());
    if (mdns_url[0] != '\0') {
        cJSON_AddStringToObject(root, "mdnsUrl", mdns_url);
    }

    cJSON *cap = cJSON_AddObjectToObject(root, "cap");
    if (cap) {
        cJSON_AddBoolToObject(cap, "wifiScan", true);
        cJSON_AddBoolToObject(cap, "mdns", wifi_admin_is_mdns_ready());
        cJSON_AddNumberToObject(cap, "httpPort", 80);
    }
    return root;
}

static esp_err_t bleprov_notify_text(const char *text)
{
    if (!text || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_notify_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total_len = strlen(text);
    uint8_t msg_id = ++s_next_notify_msg_id;
    size_t offset = 0;

    do {
        size_t chunk_len = total_len - offset;
        if (chunk_len > BLEPROV_PACKET_PAYLOAD_MAX) {
            chunk_len = BLEPROV_PACKET_PAYLOAD_MAX;
        }

        uint8_t packet[4 + BLEPROV_PACKET_PAYLOAD_MAX] = {
            BLEPROV_PROTO_VER,
            0,
            msg_id,
            0x00,
        };
        if (offset == 0) {
            packet[1] |= 0x01;
        }
        if (offset + chunk_len >= total_len) {
            packet[1] |= 0x02;
        }
        if (chunk_len > 0) {
            memcpy(packet + 4, text + offset, chunk_len);
        }

        struct os_mbuf *om = os_msys_get_pkthdr((uint16_t)(4 + chunk_len), 0);
        if (!om) {
            return ESP_ERR_NO_MEM;
        }
        int rc = os_mbuf_append(om, packet, (uint16_t)(4 + chunk_len));
        if (rc != 0) {
            os_mbuf_free_chain(om);
            return ESP_FAIL;
        }
        rc = ble_gatts_notify_custom(s_conn_handle, s_notify_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify failed rc=%d", rc);
            return ESP_FAIL;
        }

        offset += chunk_len;
    } while (offset < total_len || total_len == 0);

    return ESP_OK;
}

static esp_err_t bleprov_notify_json(cJSON *root)
{
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    bleprov_log_json_payload("ble tx", root);

    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = bleprov_notify_text(text);
    cJSON_free(text);
    return err;
}

static void bleprov_send_simple_result(const char *type,
                                       const char *request_id,
                                       bool ok,
                                       const char *code,
                                       const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "type", type);
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "requestId", request_id);
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    if (code && code[0] != '\0') {
        cJSON_AddStringToObject(root, "code", code);
    }
    if (message && message[0] != '\0') {
        cJSON_AddStringToObject(root, "message", message);
    }

    (void)bleprov_notify_json(root);
    cJSON_Delete(root);
}

static void bleprov_send_progress(const char *step, int percent, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "type", "provision_progress");
    cJSON_AddStringToObject(root, "step", step);
    cJSON_AddNumberToObject(root, "percent", percent);
    if (message && message[0] != '\0') {
        cJSON_AddStringToObject(root, "message", message);
    }

    (void)bleprov_notify_json(root);
    cJSON_Delete(root);
}

static void bleprov_send_hello_rsp(const char *request_id)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "type", "hello_rsp");
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "requestId", request_id);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "deviceId", wifi_admin_get_device_id());
    cJSON_AddStringToObject(root, "deviceName", wifi_admin_get_device_name());
    cJSON_AddStringToObject(root, "provisionState", bleprov_provision_state());
    cJSON_AddNumberToObject(root, "httpPort", 80);

    (void)bleprov_notify_json(root);
    cJSON_Delete(root);
}

static void bleprov_send_wifi_scan_rsp(const char *request_id,
                                       ephoto_wifi_scan_result_t *items,
                                       size_t count)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "type", "wifi_scan_rsp");
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "requestId", request_id);
    }
    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *array = cJSON_AddArrayToObject(root, "items");
    if (array) {
        for (size_t i = 0; i < count; ++i) {
            cJSON *entry = cJSON_CreateObject();
            if (!entry) {
                continue;
            }
            cJSON_AddStringToObject(entry, "ssid", items[i].ssid);
            cJSON_AddNumberToObject(entry, "rssi", items[i].rssi);
            cJSON_AddStringToObject(entry, "auth", bleprov_auth_mode_string(items[i].auth_mode));
            cJSON_AddItemToArray(array, entry);
        }
    }

    (void)bleprov_notify_json(root);
    cJSON_Delete(root);
}

static void bleprov_send_provision_result_ok(const char *request_id)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    char mdns_url[64] = {0};
    (void)wifi_admin_get_mdns_url(mdns_url, sizeof(mdns_url));

    cJSON_AddStringToObject(root, "type", "provision_result");
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "requestId", request_id);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "deviceId", wifi_admin_get_device_id());
    cJSON_AddStringToObject(root, "deviceName", wifi_admin_get_device_name());
    cJSON_AddStringToObject(root, "staIp", wifi_admin_get_sta_ip());
    cJSON_AddStringToObject(root, "mdnsUrl", mdns_url);
    cJSON_AddNumberToObject(root, "httpPort", 80);

    (void)bleprov_notify_json(root);
    cJSON_Delete(root);
}

static void bleprov_process_wifi_scan(const bleprov_cmd_t *cmd)
{
    ESP_LOGI(TAG,
             "wifi scan request: requestId=%s",
             cmd->request_id[0] ? cmd->request_id : "-");

    ephoto_wifi_scan_result_t items[BLEPROV_MAX_RESULTS] = {0};
    size_t count = 0;
    hosted_service_set_refresh_suspended(true);
    esp_err_t err = wifi_admin_scan_for_provisioning(items, BLEPROV_MAX_RESULTS, &count);
    hosted_service_set_refresh_suspended(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
        bleprov_send_simple_result("wifi_scan_rsp",
                                   cmd->request_id,
                                   false,
                                   "WIFI_SCAN_FAILED",
                                   "wifi scan failed");
        return;
    }

    ESP_LOGI(TAG, "wifi scan complete: count=%u", (unsigned)count);
    for (size_t i = 0; i < count; ++i) {
        ESP_LOGI(TAG,
                 "wifi scan item[%u]: ssid=%s rssi=%d auth=%s",
                 (unsigned)i,
                 items[i].ssid,
                 (int)items[i].rssi,
                 bleprov_auth_mode_string(items[i].auth_mode));
    }

    bleprov_send_wifi_scan_rsp(cmd->request_id, items, count);
}

static void bleprov_process_provision(const bleprov_cmd_t *cmd)
{
    ESP_LOGI(TAG,
             "provision request: requestId=%s ssid=%s deviceName=%s",
             cmd->request_id[0] ? cmd->request_id : "-",
             cmd->ssid,
             cmd->device_name[0] ? cmd->device_name : "-");

    s_provisioning_busy = true;
    hosted_service_set_refresh_suspended(true);
    bleprov_send_progress("received", 5, "request received");

    if (cmd->device_name[0] != '\0') {
        bleprov_send_progress("saving", 15, "saving device name");
        esp_err_t name_err = wifi_admin_set_device_name(cmd->device_name);
        if (name_err != ESP_OK) {
            ESP_LOGW(TAG, "set device name failed: %s", esp_err_to_name(name_err));
            bleprov_send_progress("failed", 100, "invalid device name");
            bleprov_send_simple_result("provision_result",
                                       cmd->request_id,
                                       false,
                                       "INVALID_REQUEST",
                                       "invalid device name");
            s_provisioning_busy = false;
            hosted_service_set_refresh_suspended(false);
            return;
        }
    }

    bleprov_send_progress("connecting_wifi", 25, "connecting to wifi");
    esp_err_t err = wifi_admin_connect(cmd->ssid, cmd->password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi connect request failed: %s", esp_err_to_name(err));
        bleprov_send_progress("failed", 100, "wifi connect request failed");
        bleprov_send_simple_result("provision_result",
                                   cmd->request_id,
                                   false,
                                   "INTERNAL_ERROR",
                                   "wifi connect request failed");
        s_provisioning_busy = false;
        hosted_service_set_refresh_suspended(false);
        return;
    }

    int64_t deadline = bleprov_now_ms() + BLEPROV_CONNECT_TIMEOUT_MS;
    bool got_ip = false;
    while (bleprov_now_ms() < deadline) {
        const char *ip = wifi_admin_get_sta_ip();
        if (wifi_admin_is_sta_connected() && ip && ip[0] != '\0') {
            got_ip = true;
            ESP_LOGI(TAG, "wifi connected: staIp=%s", ip);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!got_ip) {
        uint8_t reason = wifi_admin_get_last_disconnect_reason();
        ESP_LOGW(TAG,
                 "wifi connect timeout/fail: reason=%u code=%s message=%s",
                 (unsigned)reason,
                 bleprov_reason_code(reason),
                 bleprov_reason_message(reason));
        bleprov_send_progress("failed", 100, bleprov_reason_message(reason));
        bleprov_send_simple_result("provision_result",
                                   cmd->request_id,
                                   false,
                                   bleprov_reason_code(reason),
                                   bleprov_reason_message(reason));
        s_provisioning_busy = false;
        hosted_service_set_refresh_suspended(false);
        return;
    }

    bleprov_send_progress("got_ip", 70, "got local ip");
    bleprov_send_progress("mdns_ready", 85, "mdns is ready");

    char mdns_url[64] = {0};
    if (wifi_admin_get_mdns_url(mdns_url, sizeof(mdns_url)) == ESP_OK) {
        ESP_LOGI(TAG, "mdns ready: %s", mdns_url);
    } else {
        ESP_LOGW(TAG, "mdns url unavailable after got ip");
    }

    deadline = bleprov_now_ms() + BLEPROV_HTTP_READY_TIMEOUT_MS;
    while (bleprov_now_ms() < deadline) {
        if (web_api_is_running()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!web_api_is_running()) {
        ESP_LOGW(TAG, "http service not ready before timeout");
        bleprov_send_progress("failed", 100, "http not ready");
        bleprov_send_simple_result("provision_result",
                                   cmd->request_id,
                                   false,
                                   "HTTP_NOT_READY",
                                   "http not ready");
        s_provisioning_busy = false;
        hosted_service_set_refresh_suspended(false);
        return;
    }

    bleprov_send_progress("http_ready", 95, "http ready");
    bleprov_send_provision_result_ok(cmd->request_id);
    ESP_LOGI(TAG, "provision success: requestId=%s", cmd->request_id[0] ? cmd->request_id : "-");
    s_provisioning_busy = false;
    hosted_service_set_refresh_suspended(false);
    bleprov_refresh_state_locked();
}

static void bleprov_worker_task(void *arg)
{
    (void)arg;
    bleprov_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        switch (cmd.type) {
        case BLEPROV_CMD_WIFI_SCAN:
            bleprov_process_wifi_scan(&cmd);
            break;
        case BLEPROV_CMD_PROVISION:
            bleprov_process_provision(&cmd);
            break;
        case BLEPROV_CMD_HELLO:
        default:
            bleprov_send_hello_rsp(cmd.request_id);
            break;
        }
    }
}

static esp_err_t bleprov_enqueue_command(const bleprov_cmd_t *cmd)
{
    if (!s_cmd_queue || !cmd) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_cmd_queue, cmd, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void bleprov_start_advertising(void)
{
    if (!s_synced || !s_should_advertise || s_advertising) {
        return;
    }

    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    memset(&fields, 0, sizeof(fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = (ble_uuid128_t[]){s_service_uuid};
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 0;

    rsp_fields.name = (uint8_t *)s_ble_name;
    rsp_fields.name_len = strlen(s_ble_name);
    rsp_fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv set fields failed rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv rsp set fields failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, bleprov_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv start failed rc=%d", rc);
        return;
    }

    s_advertising = true;
    ESP_LOGI(TAG, "ble provisioning advertising: %s", s_ble_name);
}

static void bleprov_stop_advertising(void)
{
    if (!s_advertising) {
        return;
    }

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv stop failed rc=%d", rc);
        return;
    }

    s_advertising = false;
    ESP_LOGI(TAG, "ble provisioning advertising stopped");
}

static void bleprov_refresh_state_locked(void)
{
    bool should_advertise = bleprov_should_advertise();
    if (should_advertise != s_should_advertise) {
        ESP_LOGI(TAG,
                 "ble provisioning state refresh: advertise %d -> %d, wifiMode=%s, staConnected=%d, ip=%s",
                 s_should_advertise ? 1 : 0,
                 should_advertise ? 1 : 0,
                 ephoto_network_mode_to_string(wifi_admin_get_mode()),
                 wifi_admin_is_sta_connected() ? 1 : 0,
                 wifi_admin_get_sta_ip());
    }
    s_should_advertise = should_advertise;

    if (!s_started || !s_synced) {
        return;
    }

    if (s_should_advertise) {
        bleprov_start_advertising();
    } else {
        bleprov_stop_advertising();
    }
}

static void bleprov_on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset reason=%d", reason);
    s_synced = false;
    s_advertising = false;
}

static void bleprov_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed rc=%d", rc);
        return;
    }

    s_synced = true;
    ESP_LOGI(TAG,
             "ble host synced: name=%s provisionState=%s advertise=%d",
             s_ble_name,
             bleprov_provision_state(),
             s_should_advertise ? 1 : 0);
    bleprov_refresh_state_locked();
}

static void bleprov_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int bleprov_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_advertising = false;
            ESP_LOGI(TAG, "ble connected handle=%u", (unsigned)s_conn_handle);
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            bleprov_start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "ble disconnected reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_should_advertise) {
            bleprov_start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        if (s_should_advertise) {
            bleprov_start_advertising();
        }
        return 0;
    default:
        return 0;
    }
}

static int bleprov_gatt_access(uint16_t conn_handle,
                               uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt,
                               void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == s_info_handle) {
        cJSON *root = bleprov_build_info_json();
        if (!root) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        char *text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!text) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        int rc = os_mbuf_append(ctxt->om, text, (uint16_t)strlen(text));
        cJSON_free(text);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t packet_len = OS_MBUF_PKTLEN(ctxt->om);
        if (packet_len < 4 || packet_len > (4 + BLEPROV_PACKET_PAYLOAD_MAX)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t packet[4 + BLEPROV_PACKET_PAYLOAD_MAX] = {0};
        int rc = ble_hs_mbuf_to_flat(ctxt->om, packet, sizeof(packet), NULL);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (packet[0] != BLEPROV_PROTO_VER) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint8_t flags = packet[1];
        uint8_t msg_id = packet[2];
        bool first = (flags & 0x01U) != 0;
        bool last = (flags & 0x02U) != 0;
        size_t payload_len = packet_len - 4;
        int64_t now_ms = bleprov_now_ms();

        if (s_rx_state.active && (now_ms - s_rx_state.started_ms) > 5000) {
            memset(&s_rx_state, 0, sizeof(s_rx_state));
        }

        if (first) {
            memset(&s_rx_state, 0, sizeof(s_rx_state));
            s_rx_state.active = true;
            s_rx_state.msg_id = msg_id;
            s_rx_state.started_ms = now_ms;
        }

        if (!s_rx_state.active || s_rx_state.msg_id != msg_id) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (s_rx_state.len + payload_len + 1 > sizeof(s_rx_state.buffer)) {
            memset(&s_rx_state, 0, sizeof(s_rx_state));
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        if (payload_len > 0) {
            memcpy(s_rx_state.buffer + s_rx_state.len, packet + 4, payload_len);
            s_rx_state.len += payload_len;
            s_rx_state.buffer[s_rx_state.len] = '\0';
        }

        if (!last) {
            return 0;
        }

        cJSON *root = cJSON_Parse(s_rx_state.buffer);
        memset(&s_rx_state, 0, sizeof(s_rx_state));
        if (!root) {
            ESP_LOGW(TAG, "ble rx invalid json");
            bleprov_send_simple_result("error_rsp", "", false, "INVALID_REQUEST", "invalid json");
            return BLE_ATT_ERR_UNLIKELY;
        }

        bleprov_log_json_payload("ble rx", root);

        const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
        if (!cJSON_IsString(type) || !type->valuestring) {
            ESP_LOGW(TAG, "ble rx missing type");
            cJSON_Delete(root);
            bleprov_send_simple_result("error_rsp", "", false, "INVALID_REQUEST", "missing type");
            return 0;
        }

        bleprov_cmd_t cmd = {0};
        if (cJSON_IsString(request_id) && request_id->valuestring) {
            strlcpy(cmd.request_id, request_id->valuestring, sizeof(cmd.request_id));
        }

        if (strcmp(type->valuestring, "hello_req") == 0) {
            cmd.type = BLEPROV_CMD_HELLO;
        } else if (strcmp(type->valuestring, "wifi_scan_req") == 0) {
            cmd.type = BLEPROV_CMD_WIFI_SCAN;
        } else if (strcmp(type->valuestring, "provision_req") == 0) {
            const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
            const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
            const cJSON *device_name = cJSON_GetObjectItemCaseSensitive(root, "deviceName");
            if (!cJSON_IsString(ssid) || !ssid->valuestring || ssid->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "provision request missing ssid");
                cJSON_Delete(root);
                bleprov_send_simple_result("provision_result",
                                           cmd.request_id,
                                           false,
                                           "INVALID_REQUEST",
                                           "missing ssid");
                return 0;
            }
            cmd.type = BLEPROV_CMD_PROVISION;
            strlcpy(cmd.ssid, ssid->valuestring, sizeof(cmd.ssid));
            if (cJSON_IsString(password) && password->valuestring) {
                strlcpy(cmd.password, password->valuestring, sizeof(cmd.password));
            }
            if (cJSON_IsString(device_name) && device_name->valuestring) {
                strlcpy(cmd.device_name, device_name->valuestring, sizeof(cmd.device_name));
            }
        } else {
            ESP_LOGW(TAG, "unsupported ble request type: %s", type->valuestring);
            cJSON_Delete(root);
            bleprov_send_simple_result("error_rsp", cmd.request_id, false, "INVALID_REQUEST", "unsupported type");
            return 0;
        }

        cJSON_Delete(root);
        if (cmd.type == BLEPROV_CMD_PROVISION && s_provisioning_busy) {
            ESP_LOGW(TAG, "provision request rejected: busy");
            bleprov_send_simple_result("provision_result", cmd.request_id, false, "BUSY", "device busy");
            return 0;
        }
        if (bleprov_enqueue_command(&cmd) != ESP_OK) {
            ESP_LOGW(TAG, "ble command queue full");
            const char *rsp_type = cmd.type == BLEPROV_CMD_WIFI_SCAN ? "wifi_scan_rsp"
                                  : cmd.type == BLEPROV_CMD_PROVISION ? "provision_result"
                                                                      : "hello_rsp";
            bleprov_send_simple_result(rsp_type, cmd.request_id, false, "BUSY", "device busy");
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

esp_err_t ble_provisioning_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    bleprov_update_ble_name();
    s_should_advertise = bleprov_should_advertise();

    if (!s_cmd_queue) {
        s_cmd_queue = xQueueCreate(BLEPROV_QUEUE_LEN, sizeof(bleprov_cmd_t));
        if (!s_cmd_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    int hosted_rc = esp_hosted_connect_to_slave();
    if (hosted_rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_hosted_connect_to_slave rc=%d", hosted_rc);
    }

    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_init(), TAG, "bt controller init failed");
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_enable(), TAG, "bt controller enable failed");
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init failed");

    ble_hs_cfg.reset_cb = bleprov_on_reset;
    ble_hs_cfg.sync_cb = bleprov_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt count failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt add failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_svc_gap_device_name_set(s_ble_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "device name set failed rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(bleprov_host_task);
    xTaskCreatePinnedToCore(bleprov_worker_task, "bleprov_worker", 6144, NULL, 7, NULL, 0);

    s_started = true;
    ESP_LOGI(TAG, "ble provisioning service started, advertise=%d", s_should_advertise ? 1 : 0);
    return ESP_OK;
}

void ble_provisioning_refresh_state(void)
{
    bleprov_refresh_state_locked();
}
