#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t boot_image_service_init(void);
void boot_image_service_get_status(ephoto_boot_image_status_t *out_status);
esp_err_t boot_image_service_use_default(void);
esp_err_t boot_image_service_set_custom_rgb565(const char *display_name,
                                               const void *pixels,
                                               uint16_t width,
                                               uint16_t height);
bool boot_image_service_is_custom_selected(void);
esp_err_t boot_image_service_factory_reset(void);

#ifdef __cplusplus
}
#endif
