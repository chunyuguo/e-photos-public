#pragma once

#include <stdbool.h>

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_service_init(void);
// Confirms a newly OTA-booted image after the critical runtime services are live.
// It is a no-op during normal boots.
esp_err_t ota_service_confirm_boot(void);
void ota_service_reject_boot_and_reboot(void);
void ota_service_get_status(ephoto_ota_status_t *out_status);
esp_err_t ota_service_request_check(bool manual_trigger,
                                    const ephoto_settings_t *settings,
                                    bool sta_connected);
esp_err_t ota_service_request_update(bool manual_trigger,
                                     const ephoto_settings_t *settings,
                                     bool sta_connected);
void ota_service_tick(const ephoto_settings_t *settings, bool sta_connected);
void ota_service_set_runtime_busy(bool busy);

#ifdef __cplusplus
}
#endif
