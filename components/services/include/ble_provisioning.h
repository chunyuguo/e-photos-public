#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_provisioning_start(void);
void ble_provisioning_refresh_state(void);

#ifdef __cplusplus
}
#endif
