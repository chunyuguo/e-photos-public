#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*gallery_service_scan_progress_cb_t)(void *ctx, size_t completed, size_t total);

esp_err_t gallery_service_init(const board_profile_t *profile);
esp_err_t gallery_service_rescan(void);
esp_err_t gallery_service_rescan_with_progress(gallery_service_scan_progress_cb_t progress_cb, void *progress_ctx);
int gallery_service_get_count(void);
int gallery_service_get_count_filtered(ephoto_orientation_filter_t filter);
uint32_t gallery_service_get_revision(void);
size_t gallery_service_get_total_image_candidates(void);
size_t gallery_service_get_unsupported_png_count(void);
size_t gallery_service_get_unsupported_progressive_jpeg_count(void);
size_t gallery_service_get_unsupported_other_count(void);
esp_err_t gallery_service_get_item(int index, ephoto_photo_t *out_item);
esp_err_t gallery_service_get_all(ephoto_photo_t **out_items, size_t *out_count);
esp_err_t gallery_service_get_range(size_t offset,
                                    size_t limit,
                                    ephoto_photo_t **out_items,
                                    size_t *out_count,
                                    size_t *out_total);
void gallery_service_release_all(ephoto_photo_t *items);
int gallery_service_find_index_by_path(const char *path);
int gallery_service_step_index(int current_index, ephoto_playback_mode_t mode, ephoto_orientation_filter_t filter, int delta);
esp_err_t gallery_service_set_photo_manual_rotation_by_path(const char *path,
                                                            uint16_t manual_rotation_deg,
                                                            ephoto_photo_t *out_item);
esp_err_t gallery_service_reset_photo_orientation_by_path(const char *path, ephoto_photo_t *out_item);
esp_err_t gallery_service_delete_by_path(const char *path);

#ifdef __cplusplus
}
#endif
