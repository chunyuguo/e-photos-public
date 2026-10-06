#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t settings_service_init(const board_profile_t *profile);
esp_err_t settings_service_load(ephoto_settings_t *out_settings);
esp_err_t settings_service_save(const ephoto_settings_t *settings);

#ifdef __cplusplus
}
#endif
