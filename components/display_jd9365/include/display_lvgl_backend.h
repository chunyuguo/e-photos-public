#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_lvgl_backend_init(const board_profile_t *profile,
                                    esp_lcd_panel_io_handle_t io_handle,
                                    esp_lcd_panel_handle_t panel_handle,
                                    uint16_t startup_rotation_deg);
void display_lvgl_backend_set_startup_scan_progress(size_t completed, size_t total);
void display_lvgl_backend_set_software_brightness_enabled(bool enabled);
void display_lvgl_backend_release_album_resources(void);
bool display_lvgl_backend_has_pending_album_thumbnail_work(void);
void display_lvgl_backend_apply_settings(const ephoto_settings_t *settings);
void display_lvgl_backend_render_state(const ephoto_app_state_t *state,
                                       const ephoto_settings_t *effective_settings,
                                       const void *photo_pixels,
                                       bool photo_ready,
                                       bool photo_loading,
                                       const char *clock_text);

#ifdef __cplusplus
}
#endif
