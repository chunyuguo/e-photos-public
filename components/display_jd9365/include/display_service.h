#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*display_service_cache_progress_cb_t)(void *ctx,
                                                    uint32_t completed,
                                                    uint32_t failed);

esp_err_t display_service_init(const board_profile_t *profile);
void display_service_set_startup_scan_progress(size_t completed, size_t total);
void display_service_render_state(const ephoto_app_state_t *state);
void display_service_apply_settings(const ephoto_settings_t *settings);
void display_service_mark_frame_rendered(void);
bool display_service_is_busy(void);
void display_service_release_album_resources(void);
bool display_service_has_pending_album_thumbnail_work(void);
bool display_service_has_cache(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg);
bool display_service_has_known_cache_failure(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg);
esp_err_t display_service_get_cache_failure(const char *source_path,
                                            ephoto_fit_mode_t fit_mode,
                                            uint16_t rotation_deg,
                                            esp_err_t *out_error);
bool display_service_has_web_thumbnail(const char *source_path, ephoto_fit_mode_t fit_mode);
bool display_service_has_known_web_thumbnail_failure(const char *source_path, ephoto_fit_mode_t fit_mode);
esp_err_t display_service_get_web_thumbnail_failure(const char *source_path,
                                                    ephoto_fit_mode_t fit_mode,
                                                    esp_err_t *out_error);
bool display_service_should_defer_photo(const ephoto_photo_t *photo, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg);
bool display_service_is_photo_ready(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg);
esp_err_t display_service_stage_photo(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg);
esp_err_t display_service_prepare_photo_cache(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg);
esp_err_t display_service_prepare_web_thumbnail(const char *source_path, ephoto_fit_mode_t fit_mode);
esp_err_t display_service_decode_jpeg_rgb565(const uint8_t *jpeg_data,
                                             size_t jpeg_size,
                                             uint16_t max_width,
                                             uint16_t max_height,
                                             void **out_pixels,
                                             uint16_t *out_width,
                                             uint16_t *out_height);
esp_err_t display_service_decode_jpeg_file_to_screen_rgb565(const char *source_path,
                                                            ephoto_fit_mode_t fit_mode,
                                                            uint16_t rotation_deg,
                                                            void **out_pixels,
                                                            uint16_t *out_width,
                                                            uint16_t *out_height);
esp_err_t display_service_get_boot_image_rgb565(const void **out_pixels,
                                                uint16_t *out_width,
                                                uint16_t *out_height);
esp_err_t display_service_load_web_thumbnail_rgb565(const char *source_path,
                                                    ephoto_fit_mode_t fit_mode,
                                                    uint16_t max_width,
                                                    uint16_t max_height,
                                                    void **out_pixels,
                                                    uint16_t *out_width,
                                                    uint16_t *out_height);
esp_err_t display_service_prepare_missing_photo_caches(const char *source_path,
                                                       uint32_t *out_completed,
                                                       uint32_t *out_failed,
                                                       display_service_cache_progress_cb_t progress_cb,
                                                       void *progress_ctx);
esp_err_t display_service_get_cache_jpeg_info(const char *source_path,
                                              ephoto_fit_mode_t fit_mode,
                                              uint16_t rotation_deg,
                                              char *out_cache_path,
                                              size_t out_cache_path_len,
                                              size_t *out_payload_offset,
                                              size_t *out_payload_size);
esp_err_t display_service_get_web_thumbnail_jpeg_info(const char *source_path,
                                                      ephoto_fit_mode_t fit_mode,
                                                      char *out_cache_path,
                                                      size_t out_cache_path_len,
                                                      size_t *out_payload_offset,
                                                      size_t *out_payload_size);
esp_err_t display_service_invalidate_photo_cache(const char *source_path);
esp_err_t display_service_purge_all_caches(void);

#ifdef __cplusplus
}
#endif
