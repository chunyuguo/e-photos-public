#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hosted_service_init(void);
esp_err_t hosted_service_refresh(bool force);
void hosted_service_set_refresh_suspended(bool suspended);
bool hosted_service_is_ready(void);
void hosted_service_get_status(ephoto_hosted_status_t *out_status);
esp_err_t hosted_service_upgrade_embedded_if_needed(void);
void hosted_service_set_paused(bool paused);
bool hosted_service_is_paused(void);

#ifdef __cplusplus
}
#endif
