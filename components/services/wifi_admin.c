#include "wifi_admin.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "ble_provisioning.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "wifi_admin";
static const char *NAMESPACE = "wifi_admin";

static ephoto_network_mode_t s_mode = EPHOTO_NETWORK_UNAVAILABLE;
static ephoto_wifi_profile_t s_profiles[EPHOTO_MAX_WIFI_PROFILES];
static size_t s_profile_count;
static char s_current_ssid[EPHOTO_MAX_SSID_LEN];
static char s_ap_ssid[EPHOTO_MAX_SSID_LEN];
static char s_ap_password[EPHOTO_MAX_PASSWORD_LEN];
static char s_sta_ip[16];
static char s_ap_ip[16];
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_wifi_started;
static bool s_sta_connected;
static bool s_sta_connected_once_since_mode;
static bool s_mdns_ready;
static bool s_mode_switching;
static int s_retry_count;
static esp_timer_handle_t s_sta_connect_timer;
static char s_hostname[EPHOTO_MAX_HOSTNAME_LEN];
static char s_device_name[EPHOTO_MAX_DEVICE_NAME_LEN];
static char s_device_suffix[7];
static char s_device_id[10];
static bool s_mdns_http_service_registered;
static uint8_t s_last_sta_disconnect_reason;

static esp_err_t apply_mode(ephoto_network_mode_t mode);

static void ensure_device_identity(void);
static void load_device_identity(void);
static esp_err_t save_device_identity(void);
static esp_err_t ensure_mdns_http_service(void);
static esp_err_t perform_wifi_scan(ephoto_wifi_scan_result_t *out_results, size_t max_results, size_t *out_count);

#define WIFI_INITIAL_RETRY_LIMIT          8
#define WIFI_RUNTIME_RETRY_LOG_PERIOD     4
#define WIFI_STA_CONNECT_START_DELAY_MS   700U
#define WIFI_STA_RETRY_DELAY_MIN_MS       1200U
#define WIFI_STA_RETRY_DELAY_RUNTIME_MS   1800U
#define WIFI_STA_RETRY_DELAY_STEP_MS       900U
#define WIFI_STA_RETRY_DELAY_AUTH_BONUS_MS 900U
#define WIFI_STA_RETRY_DELAY_SCAN_BONUS_MS 600U
#define WIFI_STA_RETRY_DELAY_MAX_MS       8000U
#define WIFI_SCAN_ACTIVE_MIN_MS            20U
#define WIFI_SCAN_ACTIVE_MAX_MS            45U
#define WIFI_SCAN_HOME_DWELL_MS            8U
#define WIFI_SCAN_2G_BITMAP_COMMON         (WIFI_CHANNEL_1  | WIFI_CHANNEL_2  | WIFI_CHANNEL_3  | \
                                            WIFI_CHANNEL_4  | WIFI_CHANNEL_5  | WIFI_CHANNEL_6  | \
                                            WIFI_CHANNEL_7  | WIFI_CHANNEL_8  | WIFI_CHANNEL_9  | \
                                            WIFI_CHANNEL_10 | WIFI_CHANNEL_11 | WIFI_CHANNEL_12 | \
                                            WIFI_CHANNEL_13)
#if CONFIG_SLAVE_IDF_TARGET_ESP32C5
#define WIFI_SCAN_5G_BITMAP_COMMON         (WIFI_CHANNEL_36  | WIFI_CHANNEL_40  | WIFI_CHANNEL_44  | \
                                            WIFI_CHANNEL_48  | WIFI_CHANNEL_149 | WIFI_CHANNEL_153 | \
                                            WIFI_CHANNEL_157 | WIFI_CHANNEL_161)
#else
#define WIFI_SCAN_5G_BITMAP_COMMON         (1U)
#endif

static const char *wifi_reason_to_brief(unsigned reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "no_ap";
    case WIFI_REASON_AUTH_FAIL:
        return "auth_fail";
    case WIFI_REASON_ASSOC_FAIL:
        return "assoc_fail";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "hs_timeout";
    default:
        return "other";
    }
}

static bool mode_uses_sta(ephoto_network_mode_t mode)
{
    return mode == EPHOTO_NETWORK_STA;
}

static bool mode_uses_ap(ephoto_network_mode_t mode)
{
    return mode == EPHOTO_NETWORK_AP;
}

static void cancel_sta_connect_retry(void)
{
    if (!s_sta_connect_timer) {
        return;
    }

    esp_err_t err = esp_timer_stop(s_sta_connect_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stop sta connect timer failed: %s", esp_err_to_name(err));
    }
}

static uint32_t compute_sta_retry_delay_ms(bool initial_phase, int retry_count, unsigned reason)
{
    uint32_t delay_ms = initial_phase ? WIFI_STA_RETRY_DELAY_MIN_MS : WIFI_STA_RETRY_DELAY_RUNTIME_MS;
    int safe_retry = retry_count > 0 ? retry_count - 1 : 0;

    if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
        delay_ms += WIFI_STA_RETRY_DELAY_AUTH_BONUS_MS;
    } else if (reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_ASSOC_FAIL) {
        delay_ms += WIFI_STA_RETRY_DELAY_SCAN_BONUS_MS;
    }

    delay_ms += (uint32_t)safe_retry * WIFI_STA_RETRY_DELAY_STEP_MS;
    if (delay_ms > WIFI_STA_RETRY_DELAY_MAX_MS) {
        delay_ms = WIFI_STA_RETRY_DELAY_MAX_MS;
    }
    return delay_ms;
}

static void sta_connect_timer_cb(void *arg)
{
    (void)arg;

    if (s_mode_switching || !mode_uses_sta(s_mode) || s_current_ssid[0] == '\0' || s_sta_connected) {
        return;
    }

    ESP_LOGI(TAG, "connecting to ssid=%s", s_current_ssid);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

static void schedule_sta_connect_retry(uint32_t delay_ms)
{
    if (!s_sta_connect_timer) {
        return;
    }

    cancel_sta_connect_retry();
    esp_err_t err = esp_timer_start_once(s_sta_connect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start sta connect timer failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t pack_profile(const ephoto_wifi_profile_t *profile, char *packed, size_t packed_size)
{
    if (!profile || !packed || packed_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strnlen(profile->ssid, sizeof(profile->ssid));
    size_t password_len = strnlen(profile->password, sizeof(profile->password));
    size_t required = ssid_len + 1 + password_len + 1;

    if (required > packed_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(packed, profile->ssid, ssid_len);
    packed[ssid_len] = '\n';
    memcpy(packed + ssid_len + 1, profile->password, password_len);
    packed[ssid_len + 1 + password_len] = '\0';
    return ESP_OK;
}

static void format_ip(char *buffer, size_t buffer_size, const esp_netif_ip_info_t *ip_info)
{
    if (!buffer || buffer_size == 0) {
        return;
    }

    if (!ip_info || ip_info->ip.addr == 0) {
        buffer[0] = '\0';
        return;
    }

    snprintf(buffer, buffer_size, IPSTR, IP2STR(&ip_info->ip));
}

static void refresh_ip_strings(void)
{
    esp_netif_ip_info_t ip_info = {0};

    s_sta_ip[0] = '\0';
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        format_ip(s_sta_ip, sizeof(s_sta_ip), &ip_info);
    }

    s_ap_ip[0] = '\0';
    memset(&ip_info, 0, sizeof(ip_info));
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        format_ip(s_ap_ip, sizeof(s_ap_ip), &ip_info);
    }
}

static void ensure_default_ap_credentials(void)
{
    if (s_ap_ssid[0] != '\0') {
        return;
    }

    ensure_device_identity();
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "E-Photo-%s", s_device_suffix[0] ? s_device_suffix : "DEVICE");
    strlcpy(s_ap_password, "12345678", sizeof(s_ap_password));
}

static void ensure_device_identity(void)
{
    if (s_device_suffix[0] != '\0' && s_hostname[0] != '\0' && s_device_name[0] != '\0') {
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_device_suffix, sizeof(s_device_suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);

    if (s_hostname[0] == '\0') {
        snprintf(s_hostname, sizeof(s_hostname), "ephoto-%s", s_device_suffix);
    }
    if (s_device_id[0] == '\0') {
        snprintf(s_device_id, sizeof(s_device_id), "EP%s", s_device_suffix);
    }
    if (s_device_name[0] == '\0') {
        snprintf(s_device_name, sizeof(s_device_name), "E-Photo %s", s_device_suffix);
    }
}

static void load_device_identity(void)
{
    ensure_device_identity();

    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    size_t len = sizeof(s_device_name);
    if (nvs_get_str(handle, "dev_name", s_device_name, &len) != ESP_OK || s_device_name[0] == '\0') {
        s_device_name[0] = '\0';
        ensure_device_identity();
    }
    nvs_close(handle);
}

static esp_err_t save_device_identity(void)
{
    nvs_handle_t handle;
    esp_err_t ret = ESP_OK;

    ensure_device_identity();
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "open nvs failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, "dev_name", s_device_name), done, TAG, "save device name failed");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), done, TAG, "commit device identity failed");
done:
    nvs_close(handle);
    return ret;
}

static void ensure_current_profile_selected(void)
{
    if (s_current_ssid[0] == '\0' && s_profile_count > 0) {
        strlcpy(s_current_ssid, s_profiles[0].ssid, sizeof(s_current_ssid));
    }
}

static void load_profiles(void)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint8_t count = 0;
    if (nvs_get_u8(handle, "count", &count) == ESP_OK) {
        s_profile_count = count > EPHOTO_MAX_WIFI_PROFILES ? EPHOTO_MAX_WIFI_PROFILES : count;
    }

    for (size_t i = 0; i < s_profile_count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "wifi%u", (unsigned)i);
        char packed[EPHOTO_MAX_SSID_LEN + EPHOTO_MAX_PASSWORD_LEN + 2] = {0};
        size_t len = sizeof(packed);
        if (nvs_get_str(handle, key, packed, &len) != ESP_OK) {
            continue;
        }

        char *sep = strchr(packed, '\n');
        if (!sep) {
            continue;
        }
        *sep = '\0';
        strlcpy(s_profiles[i].ssid, packed, sizeof(s_profiles[i].ssid));
        strlcpy(s_profiles[i].password, sep + 1, sizeof(s_profiles[i].password));
    }

    size_t len = sizeof(s_current_ssid);
    if (nvs_get_str(handle, "current", s_current_ssid, &len) != ESP_OK) {
        s_current_ssid[0] = '\0';
    }
    nvs_close(handle);
}

static esp_err_t save_profiles(void)
{
    nvs_handle_t handle;
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "open nvs failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "count", s_profile_count), done, TAG, "set count failed");

    for (size_t i = 0; i < EPHOTO_MAX_WIFI_PROFILES; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "wifi%u", (unsigned)i);
        if (i < s_profile_count) {
            char packed[EPHOTO_MAX_SSID_LEN + EPHOTO_MAX_PASSWORD_LEN + 2];
            ESP_GOTO_ON_ERROR(pack_profile(&s_profiles[i], packed, sizeof(packed)), done, TAG, "pack profile failed");
            ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, packed), done, TAG, "set profile failed");
        } else {
            nvs_erase_key(handle, key);
        }
    }

    ESP_GOTO_ON_ERROR(nvs_set_str(handle, "current", s_current_ssid), done, TAG, "set current failed");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), done, TAG, "commit failed");
done:
    nvs_close(handle);
    return ret;
}

static const ephoto_wifi_profile_t *find_profile(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        return NULL;
    }

    for (size_t i = 0; i < s_profile_count; ++i) {
        if (strcmp(s_profiles[i].ssid, ssid) == 0) {
            return &s_profiles[i];
        }
    }
    return NULL;
}

static esp_err_t ensure_mdns_started(void)
{
    ensure_device_identity();

    if (s_mdns_ready) {
        ESP_RETURN_ON_ERROR(mdns_hostname_set(s_hostname), TAG, "mdns hostname update failed");
        ESP_RETURN_ON_ERROR(mdns_instance_name_set(s_device_name), TAG, "mdns instance update failed");
        return ESP_OK;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(mdns_hostname_set(s_hostname), TAG, "mdns hostname failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(s_device_name), TAG, "mdns instance failed");
    s_mdns_ready = true;
    return ensure_mdns_http_service();
}

static esp_err_t ensure_mdns_http_service(void)
{
    if (!s_mdns_ready || s_mdns_http_service_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0),
                        TAG,
                        "mdns http service add failed");
    s_mdns_http_service_registered = true;
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            s_retry_count = 0;
            if (mode_uses_sta(s_mode) && s_current_ssid[0]) {
                schedule_sta_connect_retry(WIFI_STA_CONNECT_START_DELAY_MS);
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
            unsigned reason = event ? (unsigned)event->reason : 0U;
            s_last_sta_disconnect_reason = (uint8_t)reason;
            s_sta_connected = false;
            s_sta_ip[0] = '\0';
            if (s_mode_switching) {
                refresh_ip_strings();
                break;
            }
            if (mode_uses_sta(s_mode) && s_current_ssid[0]) {
                ++s_retry_count;
                if (!s_sta_connected_once_since_mode) {
                    if (s_retry_count <= WIFI_INITIAL_RETRY_LIMIT) {
                        uint32_t delay_ms = compute_sta_retry_delay_ms(true, s_retry_count, reason);
                        ESP_LOGW(TAG,
                                 "initial wifi connect failed for ssid=%s, retry %d/%d in %ums, reason=%u(%s)",
                                 s_current_ssid,
                                 s_retry_count,
                                 WIFI_INITIAL_RETRY_LIMIT,
                                 (unsigned)delay_ms,
                                 reason,
                                 wifi_reason_to_brief(reason));
                        schedule_sta_connect_retry(delay_ms);
                    } else {
                        ESP_LOGW(TAG,
                                 "initial wifi connect failed for ssid=%s after %d retries, fallback to AP, reason=%u(%s)",
                                 s_current_ssid,
                                 WIFI_INITIAL_RETRY_LIMIT,
                                 reason,
                                 wifi_reason_to_brief(reason));
                        if (apply_mode(EPHOTO_NETWORK_AP) == ESP_OK) {
                            refresh_ip_strings();
                        }
                    }
                } else {
                    uint32_t delay_ms = compute_sta_retry_delay_ms(false, s_retry_count, reason);
                    if (s_retry_count == 1 || (s_retry_count % WIFI_RUNTIME_RETRY_LOG_PERIOD) == 0) {
                        ESP_LOGW(TAG,
                                 "wifi runtime disconnect from ssid=%s, retry=%d in %ums, reason=%u(%s)",
                                 s_current_ssid,
                                 s_retry_count,
                                 (unsigned)delay_ms,
                                 reason,
                                 wifi_reason_to_brief(reason));
                    }
                    schedule_sta_connect_retry(delay_ms);
                }
            } else {
                ESP_LOGW(TAG,
                         "wifi disconnected from ssid=%s, reason=%u(%s)",
                         s_current_ssid,
                         reason,
                         wifi_reason_to_brief(reason));
            }
            ble_provisioning_refresh_state();
            break;
        }
        case WIFI_EVENT_AP_START:
        case WIFI_EVENT_AP_STOP:
            refresh_ip_strings();
            ble_provisioning_refresh_state();
            break;
        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connected = true;
        s_last_sta_disconnect_reason = 0;
        s_sta_connected_once_since_mode = true;
        s_retry_count = 0;
        cancel_sta_connect_retry();
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        refresh_ip_strings();
        ESP_LOGI(TAG, "got sta ip: %s", s_sta_ip);
        ble_provisioning_refresh_state();
    }
}

static esp_err_t apply_mode(ephoto_network_mode_t mode)
{
    wifi_mode_t wifi_mode = WIFI_MODE_NULL;
    esp_err_t ret = ESP_OK;

    ensure_default_ap_credentials();
    ensure_current_profile_selected();

    switch (mode) {
    case EPHOTO_NETWORK_AP:
        wifi_mode = WIFI_MODE_AP;
        break;
    case EPHOTO_NETWORK_STA:
        wifi_mode = WIFI_MODE_STA;
        break;
    case EPHOTO_NETWORK_DISCONNECTED:
        wifi_mode = WIFI_MODE_NULL;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    s_mode_switching = true;
    cancel_sta_connect_retry();

    if (s_wifi_started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ret = stop_err;
            goto done;
        }
        s_wifi_started = false;
        s_sta_connected = false;
        s_sta_connected_once_since_mode = false;
        s_sta_ip[0] = '\0';
        s_ap_ip[0] = '\0';
    }

    if (wifi_mode == WIFI_MODE_NULL) {
        s_mode = EPHOTO_NETWORK_DISCONNECTED;
        s_sta_connected_once_since_mode = false;
        refresh_ip_strings();
        goto done;
    }

    wifi_config_t ap_config = {0};
    wifi_config_t sta_config = {0};

    if (mode_uses_ap(mode)) {
        strlcpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid));
        strlcpy((char *)ap_config.ap.password, s_ap_password, sizeof(ap_config.ap.password));
        ap_config.ap.ssid_len = strlen(s_ap_ssid);
        ap_config.ap.max_connection = 4;
        ap_config.ap.channel = 1;
        ap_config.ap.authmode = strlen(s_ap_password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    }

    if (mode_uses_sta(mode) && s_current_ssid[0]) {
        const ephoto_wifi_profile_t *profile = find_profile(s_current_ssid);
        if (profile) {
            strlcpy((char *)sta_config.sta.ssid, profile->ssid, sizeof(sta_config.sta.ssid));
            strlcpy((char *)sta_config.sta.password, profile->password, sizeof(sta_config.sta.password));
            sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
            sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
            sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        } else if (mode == EPHOTO_NETWORK_STA) {
            ESP_LOGW(TAG, "current ssid missing from saved profiles, fallback to AP");
            mode = EPHOTO_NETWORK_AP;
            wifi_mode = WIFI_MODE_AP;
        }
    } else if (mode == EPHOTO_NETWORK_STA) {
        ESP_LOGW(TAG, "no saved ssid, fallback to AP");
        mode = EPHOTO_NETWORK_AP;
        wifi_mode = WIFI_MODE_AP;
    }

    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(wifi_mode), done, TAG, "set mode failed");
    if (mode_uses_ap(mode)) {
        ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), done, TAG, "set ap config failed");
    }
    if (mode_uses_sta(mode) && s_current_ssid[0]) {
        ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), done, TAG, "set sta config failed");
    }
    ESP_GOTO_ON_ERROR(esp_wifi_start(), done, TAG, "wifi start failed");
    s_wifi_started = true;
    s_retry_count = 0;
    s_sta_connected_once_since_mode = false;
    s_mode = mode;
    refresh_ip_strings();
    ESP_LOGI(TAG,
             "wifi mode applied: %s ap=%s sta=%s",
             ephoto_network_mode_to_string(s_mode),
             s_ap_ssid,
             s_current_ssid[0] ? s_current_ssid : "-");
done:
    s_mode_switching = false;
    ble_provisioning_refresh_state();
    return ret;
}

esp_err_t wifi_admin_init(void)
{
    ensure_default_ap_credentials();
    load_device_identity();
    load_profiles();
    ensure_current_profile_selected();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable wifi power save failed");
    if (!s_sta_connect_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = &sta_connect_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_sta_connect",
            .skip_unhandled_events = true,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_sta_connect_timer), TAG, "sta connect timer create failed");
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG,
                        "wifi handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                        TAG,
                        "ip handler register failed");
    ESP_RETURN_ON_ERROR(ensure_mdns_started(), TAG, "mdns init failed");

    ephoto_network_mode_t initial_mode = s_current_ssid[0] ? EPHOTO_NETWORK_STA : EPHOTO_NETWORK_AP;
    ESP_RETURN_ON_ERROR(apply_mode(initial_mode), TAG, "initial wifi mode failed");

    ESP_LOGI(TAG,
             "wifi admin initialized with %u saved profiles, host=%s, name=%s, ap=%s, ps=off",
             (unsigned)s_profile_count,
             s_hostname,
             s_device_name,
             s_ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_admin_set_mode(ephoto_network_mode_t mode)
{
    return apply_mode(mode);
}

ephoto_network_mode_t wifi_admin_get_mode(void)
{
    return s_mode;
}

esp_err_t wifi_admin_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t found = EPHOTO_MAX_WIFI_PROFILES;
    for (size_t i = 0; i < s_profile_count; ++i) {
        if (strcmp(s_profiles[i].ssid, ssid) == 0) {
            found = i;
            break;
        }
    }

    if (found == EPHOTO_MAX_WIFI_PROFILES) {
        if (s_profile_count >= EPHOTO_MAX_WIFI_PROFILES) {
            found = s_profile_count - 1;
        } else {
            found = s_profile_count++;
        }
    }

    strlcpy(s_profiles[found].ssid, ssid, sizeof(s_profiles[found].ssid));
    if (password && password[0]) {
        strlcpy(s_profiles[found].password, password, sizeof(s_profiles[found].password));
    }
    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
    ESP_RETURN_ON_ERROR(save_profiles(), TAG, "save profile failed");

    return apply_mode(EPHOTO_NETWORK_STA);
}

esp_err_t wifi_admin_remove(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_profile_count; ++i) {
        if (strcmp(s_profiles[i].ssid, ssid) != 0) {
            continue;
        }

        for (size_t j = i; j + 1 < s_profile_count; ++j) {
            s_profiles[j] = s_profiles[j + 1];
        }
        memset(&s_profiles[s_profile_count - 1], 0, sizeof(s_profiles[s_profile_count - 1]));
        --s_profile_count;

        if (strcmp(s_current_ssid, ssid) == 0) {
            s_current_ssid[0] = '\0';
            ensure_current_profile_selected();
        }

        ESP_RETURN_ON_ERROR(save_profiles(), TAG, "save profiles failed");

        return apply_mode(s_current_ssid[0] ? EPHOTO_NETWORK_STA : EPHOTO_NETWORK_AP);
    }

    return ESP_ERR_NOT_FOUND;
}

size_t wifi_admin_get_profiles(ephoto_wifi_profile_t *out_profiles, size_t max_profiles)
{
    size_t copied = s_profile_count < max_profiles ? s_profile_count : max_profiles;
    if (out_profiles && copied > 0) {
        memcpy(out_profiles, s_profiles, copied * sizeof(ephoto_wifi_profile_t));
    }
    return copied;
}

const char *wifi_admin_get_current_ssid(void)
{
    return s_current_ssid;
}

const char *wifi_admin_get_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *wifi_admin_get_ap_password(void)
{
    return s_ap_password;
}

const char *wifi_admin_get_sta_ip(void)
{
    refresh_ip_strings();
    return s_sta_ip;
}

const char *wifi_admin_get_ap_ip(void)
{
    refresh_ip_strings();
    return s_ap_ip;
}

const char *wifi_admin_get_hostname(void)
{
    ensure_device_identity();
    return s_hostname;
}

const char *wifi_admin_get_device_suffix(void)
{
    ensure_device_identity();
    return s_device_suffix;
}

const char *wifi_admin_get_device_id(void)
{
    ensure_device_identity();
    return s_device_id;
}

const char *wifi_admin_get_device_name(void)
{
    ensure_device_identity();
    return s_device_name;
}

esp_err_t wifi_admin_get_mdns_url(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ensure_device_identity();
    if (!s_mdns_ready || s_hostname[0] == '\0') {
        buffer[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(buffer, buffer_size, "http://%s.local", s_hostname);
    if (written < 0 || (size_t)written >= buffer_size) {
        buffer[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t wifi_admin_set_device_name(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }

    char cleaned[EPHOTO_MAX_DEVICE_NAME_LEN];
    size_t write = 0;
    bool last_was_space = true;
    for (size_t i = 0; name[i] != '\0' && write + 1 < sizeof(cleaned); ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20) {
            continue;
        }
        if (ch == ' ') {
            if (last_was_space) {
                continue;
            }
            cleaned[write++] = ' ';
            last_was_space = true;
            continue;
        }
        cleaned[write++] = (char)ch;
        last_was_space = false;
    }
    while (write > 0 && cleaned[write - 1] == ' ') {
        --write;
    }
    cleaned[write] = '\0';

    if (cleaned[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_device_name, cleaned, sizeof(s_device_name));
    ESP_RETURN_ON_ERROR(save_device_identity(), TAG, "save device identity failed");
    if (s_mdns_ready) {
        ESP_RETURN_ON_ERROR(mdns_instance_name_set(s_device_name), TAG, "mdns instance update failed");
    }
    return ESP_OK;
}

bool wifi_admin_has_saved_profiles(void)
{
    return s_profile_count > 0;
}

bool wifi_admin_is_sta_connected(void)
{
    return s_sta_connected;
}

bool wifi_admin_is_mdns_ready(void)
{
    return s_mdns_ready;
}

uint8_t wifi_admin_get_last_disconnect_reason(void)
{
    return s_last_sta_disconnect_reason;
}

int8_t wifi_admin_get_sta_rssi(void)
{
#if CONFIG_SLAVE_IDF_TARGET_ESP32C5 || CONFIG_SLAVE_IDF_TARGET_ESP32C6
    /*
     * ESP-Hosted's synchronous get-ap-info RPC is not required by the
     * application and can panic on some P4 + co-processor combinations.
     * Status polling is frequent, so keep RSSI unavailable on both C5 and C6
     * until this RPC path is proven stable on every supported Hosted image.
     */
    return 0;
#else
    wifi_ap_record_t ap_info = {0};
    if (!s_sta_connected) {
        return 0;
    }
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.rssi;
#endif
}

static esp_err_t perform_wifi_scan(ephoto_wifi_scan_result_t *out_results, size_t max_results, size_t *out_count)
{
    if (out_count) {
        *out_count = 0;
    }

    if (!out_results || max_results == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = WIFI_SCAN_ACTIVE_MIN_MS,
            .max = WIFI_SCAN_ACTIVE_MAX_MS,
        },
        .home_chan_dwell_time = WIFI_SCAN_HOME_DWELL_MS,
        .channel_bitmap = {
            .ghz_2_channels = WIFI_SCAN_2G_BITMAP_COMMON,
            .ghz_5_channels = WIFI_SCAN_5G_BITMAP_COMMON,
        },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "wifi scan failed");

    uint16_t ap_count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_count), TAG, "get ap num failed");
    if (ap_count == 0) {
        return ESP_OK;
    }

    size_t copied = 0;
    esp_err_t ret = ESP_OK;
    for (uint16_t i = 0; i < ap_count; ++i) {
        wifi_ap_record_t record = {0};
        ret = esp_wifi_scan_get_ap_record(&record);
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ret = ESP_OK;
                break;
            }
            break;
        }

        if (record.ssid[0] == '\0' || copied >= max_results) {
            continue;
        }
        strlcpy(out_results[copied].ssid, (const char *)record.ssid, sizeof(out_results[copied].ssid));
        out_results[copied].rssi = record.rssi;
        out_results[copied].auth_mode = (uint8_t)record.authmode;
        ++copied;
    }

    esp_err_t clear_ret = esp_wifi_clear_ap_list();
    if (clear_ret != ESP_OK && clear_ret != ESP_ERR_INVALID_STATE && ret == ESP_OK) {
        ret = clear_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    if (out_count) {
        *out_count = copied;
    }
    return ESP_OK;
}

esp_err_t wifi_admin_scan(ephoto_wifi_scan_result_t *out_results, size_t max_results, size_t *out_count)
{
    if (out_count) {
        *out_count = 0;
    }

    if (!out_results || max_results == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mode_uses_sta(s_mode)) {
        return perform_wifi_scan(out_results, max_results, out_count);
    }

    ESP_LOGW(TAG,
             "wifi scan rejected in mode=%s to keep current web/ap session stable",
             ephoto_network_mode_to_string(s_mode));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_admin_scan_for_provisioning(ephoto_wifi_scan_result_t *out_results,
                                           size_t max_results,
                                           size_t *out_count)
{
    if (out_count) {
        *out_count = 0;
    }

    if (!out_results || max_results == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // A provisioning scan can be requested while STA is still connecting to a
    // remembered network. In that window esp_wifi_scan_start() returns
    // ESP_ERR_WIFI_STATE, so we fall back to a temporary dedicated scan mode
    // unless STA is already fully connected.
    if (mode_uses_sta(s_mode) && s_sta_connected) {
        return perform_wifi_scan(out_results, max_results, out_count);
    }

    ephoto_network_mode_t previous_mode = s_mode;
    bool previous_wifi_started = s_wifi_started;
    bool previous_sta_connected = s_sta_connected;
    bool previous_sta_connected_once = s_sta_connected_once_since_mode;
    uint8_t previous_disconnect_reason = s_last_sta_disconnect_reason;
    esp_err_t scan_ret = ESP_OK;
    esp_err_t restore_ret = ESP_OK;

    s_mode_switching = true;
    cancel_sta_connect_retry();

    if (previous_wifi_started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            s_mode_switching = false;
            return stop_err;
        }
        s_wifi_started = false;
    }

    scan_ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (scan_ret != ESP_OK) {
        goto restore;
    }
    scan_ret = esp_wifi_start();
    if (scan_ret != ESP_OK) {
        goto restore;
    }
    s_wifi_started = true;
    vTaskDelay(pdMS_TO_TICKS(120));
    scan_ret = perform_wifi_scan(out_results, max_results, out_count);

restore:
    if (s_wifi_started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED && restore_ret == ESP_OK) {
            restore_ret = stop_err;
        }
        s_wifi_started = false;
    }

    if (previous_mode == EPHOTO_NETWORK_DISCONNECTED) {
        s_mode = EPHOTO_NETWORK_DISCONNECTED;
        s_sta_connected = previous_sta_connected;
        s_sta_connected_once_since_mode = previous_sta_connected_once;
        s_last_sta_disconnect_reason = previous_disconnect_reason;
        refresh_ip_strings();
    } else {
        restore_ret = apply_mode(previous_mode);
    }

    s_mode_switching = false;
    if (scan_ret != ESP_OK) {
        return scan_ret;
    }
    return restore_ret;
}
