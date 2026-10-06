#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ephoto_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool available;
    bool valid;
    uint16_t millivolts;
    uint8_t percent;
} ambient_light_status_t;

esp_err_t ambient_light_service_init(const board_profile_t *profile);
esp_err_t ambient_light_service_poll(ambient_light_status_t *out_status);
void ambient_light_service_get_status(ambient_light_status_t *out_status);

#ifdef __cplusplus
}
#endif
