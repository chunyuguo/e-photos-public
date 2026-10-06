#include "lan_discovery_service.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wifi_admin.h"

static const char *TAG = "lan_discovery";

#define EPHOTO_DISCOVERY_PORT              19026
#define EPHOTO_DISCOVERY_TASK_STACK        4096
#define EPHOTO_DISCOVERY_TASK_PRIO         4
#define EPHOTO_DISCOVERY_RECV_BUF_LEN      512
#define EPHOTO_DISCOVERY_SEND_BUF_LEN      768
#define EPHOTO_DISCOVERY_SOCKET_TIMEOUT_MS 1000
#define EPHOTO_DISCOVERY_ERROR_BACKOFF_MS  1500
#define EPHOTO_DISCOVERY_RATE_LIMIT_MS      300

static TaskHandle_t s_task_handle;
static int64_t s_last_reply_ms;
static uint32_t s_last_reply_addr;
static uint16_t s_last_reply_port;

static bool discovery_request_matches(const char *payload, size_t len)
{
    if (!payload || len == 0) {
        return false;
    }

    if (len == strlen("EPHOTO_DISCOVER_V1") &&
        memcmp(payload, "EPHOTO_DISCOVER_V1", len) == 0) {
        return true;
    }
    if (len == strlen("EPHOTO_DISCOVER") &&
        memcmp(payload, "EPHOTO_DISCOVER", len) == 0) {
        return true;
    }

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        if (strstr(payload, "discover") || strstr(payload, "DISCOVER")) {
            return true;
        }
        return false;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *proto = cJSON_GetObjectItemCaseSensitive(root, "proto");
    bool matched = cJSON_IsString(type) &&
                   type->valuestring &&
                   (strcmp(type->valuestring, "ephoto-discover") == 0 ||
                    strcmp(type->valuestring, "ephoto_discover") == 0 ||
                    strcmp(type->valuestring, "discover") == 0 ||
                    strcmp(type->valuestring, "hello") == 0) &&
                   (!cJSON_IsNumber(proto) || proto->valueint == 1);
    cJSON_Delete(root);
    return matched;
}

static bool endpoint_rate_limited(const struct sockaddr_in *remote_addr)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t addr = remote_addr ? remote_addr->sin_addr.s_addr : 0;
    uint16_t port = remote_addr ? remote_addr->sin_port : 0;

    if (addr == s_last_reply_addr &&
        port == s_last_reply_port &&
        (now_ms - s_last_reply_ms) < EPHOTO_DISCOVERY_RATE_LIMIT_MS) {
        return true;
    }

    s_last_reply_ms = now_ms;
    s_last_reply_addr = addr;
    s_last_reply_port = port;
    return false;
}

static const char *resolve_primary_ip(void)
{
    const char *sta_ip = wifi_admin_get_sta_ip();
    if (wifi_admin_is_sta_connected() && sta_ip && sta_ip[0]) {
        return sta_ip;
    }

    const char *ap_ip = wifi_admin_get_ap_ip();
    if (ap_ip && ap_ip[0]) {
        return ap_ip;
    }

    return NULL;
}

static bool build_discovery_response(char *buffer, size_t buffer_len)
{
    const char *ip = resolve_primary_ip();
    if (!buffer || buffer_len == 0 || !ip || !ip[0]) {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return false;
    }

    char mdns_url[64] = {0};
    char primary_url[64] = {0};
    const char *hostname = wifi_admin_get_hostname();
    bool initialized = wifi_admin_has_saved_profiles() || wifi_admin_is_sta_connected();

    (void)wifi_admin_get_mdns_url(mdns_url, sizeof(mdns_url));
    snprintf(primary_url, sizeof(primary_url), "http://%s", ip);

    cJSON_AddStringToObject(root, "type", "ephoto-discover-rsp");
    cJSON_AddNumberToObject(root, "proto", 1);
    cJSON_AddStringToObject(root, "product", "e-photo");
    cJSON_AddStringToObject(root, "model", EPHOTO_DISCOVERY_MODEL);
    cJSON_AddStringToObject(root, "deviceId", wifi_admin_get_device_id());
    cJSON_AddStringToObject(root, "deviceName", wifi_admin_get_device_name());
    cJSON_AddStringToObject(root, "hostname", hostname ? hostname : "");
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "primaryUrl", primary_url);
    cJSON_AddStringToObject(root, "mdnsUrl", mdns_url);
    cJSON_AddStringToObject(root, "fwVersion", EPHOTO_PROJECT_VER);
    cJSON_AddStringToObject(root, "networkMode", ephoto_network_mode_to_string(wifi_admin_get_mode()));
    cJSON_AddStringToObject(root, "macSuffix", wifi_admin_get_device_suffix());
    cJSON_AddBoolToObject(root, "staConnected", wifi_admin_is_sta_connected());
    cJSON_AddBoolToObject(root, "initialized", initialized);
    cJSON_AddNumberToObject(root, "httpPort", 80);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return false;
    }

    size_t json_len = strlen(json);
    bool ok = json_len < buffer_len;
    if (ok) {
        memcpy(buffer, json, json_len + 1);
    }
    free(json);
    return ok;
}

static int open_discovery_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket create failed: errno=%d", errno);
        return -1;
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        ESP_LOGW(TAG, "setsockopt reuseaddr failed: errno=%d", errno);
    }

    struct timeval timeout = {
        .tv_sec = EPHOTO_DISCOVERY_SOCKET_TIMEOUT_MS / 1000,
        .tv_usec = (EPHOTO_DISCOVERY_SOCKET_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG, "setsockopt rcvtimeo failed: errno=%d", errno);
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(EPHOTO_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGW(TAG, "socket bind failed: errno=%d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "udp discovery listening on port %d", EPHOTO_DISCOVERY_PORT);
    return sock;
}

static void discovery_task(void *arg)
{
    (void)arg;

    char recv_buf[EPHOTO_DISCOVERY_RECV_BUF_LEN];
    char send_buf[EPHOTO_DISCOVERY_SEND_BUF_LEN];

    while (true) {
        int sock = open_discovery_socket();
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(EPHOTO_DISCOVERY_ERROR_BACKOFF_MS));
            continue;
        }

        while (true) {
            struct sockaddr_in remote_addr = {0};
            socklen_t addr_len = sizeof(remote_addr);
            int received = recvfrom(sock,
                                    recv_buf,
                                    sizeof(recv_buf) - 1,
                                    0,
                                    (struct sockaddr *)&remote_addr,
                                    &addr_len);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                ESP_LOGW(TAG, "recvfrom failed: errno=%d", errno);
                break;
            }

            if (received == 0) {
                continue;
            }

            recv_buf[received] = '\0';
            ESP_LOGI(TAG,
                     "discover probe from %s:%u len=%d payload=%.*s",
                     inet_ntoa(remote_addr.sin_addr),
                     (unsigned)ntohs(remote_addr.sin_port),
                     received,
                     received > 96 ? 96 : received,
                     recv_buf);
            if (!discovery_request_matches(recv_buf, (size_t)received)) {
                ESP_LOGW(TAG, "probe ignored: payload not matched");
                continue;
            }
            if (endpoint_rate_limited(&remote_addr)) {
                continue;
            }
            if (!build_discovery_response(send_buf, sizeof(send_buf))) {
                ESP_LOGW(TAG, "probe matched but response build failed");
                continue;
            }

            int sent = sendto(sock,
                              send_buf,
                              strlen(send_buf),
                              0,
                              (struct sockaddr *)&remote_addr,
                              sizeof(remote_addr));
            if (sent < 0) {
                ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
            } else {
                ESP_LOGI(TAG,
                         "discover response sent to %s:%u",
                         inet_ntoa(remote_addr.sin_addr),
                         (unsigned)ntohs(remote_addr.sin_port));
            }
        }

        close(sock);
        vTaskDelay(pdMS_TO_TICKS(EPHOTO_DISCOVERY_ERROR_BACKOFF_MS));
    }
}

esp_err_t lan_discovery_service_start(void)
{
    if (s_task_handle) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(discovery_task,
                                            "lan_discovery",
                                            EPHOTO_DISCOVERY_TASK_STACK,
                                            NULL,
                                            EPHOTO_DISCOVERY_TASK_PRIO,
                                            &s_task_handle,
                                            0);
    if (ok != pdPASS) {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
