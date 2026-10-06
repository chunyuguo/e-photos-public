#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool factory_reset_service_is_running(void);
esp_err_t factory_reset_service_run(void);
void factory_reset_service_schedule_reboot(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
