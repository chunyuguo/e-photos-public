#pragma once

#include "ephoto_types.h"
#include "input_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*web_state_snapshot_fn_t)(ephoto_app_state_t *out_state);

esp_err_t web_api_start(web_state_snapshot_fn_t snapshot_fn, input_command_handler_t handler);
bool web_api_is_running(void);

#ifdef __cplusplus
}
#endif
