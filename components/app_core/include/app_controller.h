#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_controller_init(const board_profile_t *profile);
esp_err_t app_controller_start(void);
esp_err_t app_controller_submit(const ephoto_command_t *command);
void app_controller_snapshot(ephoto_app_state_t *out_state);

#ifdef __cplusplus
}
#endif
