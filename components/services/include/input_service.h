#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*input_command_handler_t)(const ephoto_command_t *command);

esp_err_t input_service_init(const board_profile_t *profile, input_command_handler_t handler);

#ifdef __cplusplus
}
#endif
