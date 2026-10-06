#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_admin_init(void);
esp_err_t wifi_admin_set_mode(ephoto_network_mode_t mode);
ephoto_network_mode_t wifi_admin_get_mode(void);
esp_err_t wifi_admin_connect(const char *ssid, const char *password);
esp_err_t wifi_admin_remove(const char *ssid);
size_t wifi_admin_get_profiles(ephoto_wifi_profile_t *out_profiles, size_t max_profiles);
esp_err_t wifi_admin_scan(ephoto_wifi_scan_result_t *out_results, size_t max_results, size_t *out_count);
esp_err_t wifi_admin_scan_for_provisioning(ephoto_wifi_scan_result_t *out_results,
                                           size_t max_results,
                                           size_t *out_count);
const char *wifi_admin_get_current_ssid(void);
const char *wifi_admin_get_ap_ssid(void);
const char *wifi_admin_get_ap_password(void);
const char *wifi_admin_get_sta_ip(void);
const char *wifi_admin_get_ap_ip(void);
const char *wifi_admin_get_hostname(void);
const char *wifi_admin_get_device_suffix(void);
const char *wifi_admin_get_device_id(void);
const char *wifi_admin_get_device_name(void);
esp_err_t wifi_admin_get_mdns_url(char *buffer, size_t buffer_size);
esp_err_t wifi_admin_set_device_name(const char *name);
bool wifi_admin_has_saved_profiles(void);
bool wifi_admin_is_sta_connected(void);
bool wifi_admin_is_mdns_ready(void);
uint8_t wifi_admin_get_last_disconnect_reason(void);
int8_t wifi_admin_get_sta_rssi(void);

#ifdef __cplusplus
}
#endif
