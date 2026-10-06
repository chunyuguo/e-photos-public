#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    size_t bytes_tested;
    int64_t write_ms;
    int64_t read_ms;
    uint32_t write_kib_per_s;
    uint32_t read_kib_per_s;
    esp_err_t error;
} ephoto_storage_benchmark_t;

esp_err_t storage_service_init(const board_profile_t *profile);
void storage_service_deinit(void);
esp_err_t storage_service_refresh_stats(void);
void storage_service_get_status(ephoto_storage_status_t *out_status);
const char *storage_service_get_mount_path(void);
const char *storage_service_get_photo_dir(void);
int storage_service_get_mounted_bus_width(void);
int storage_service_get_mounted_freq_khz(void);
esp_err_t storage_service_run_benchmark(ephoto_storage_benchmark_t *out_result);
esp_err_t storage_service_remount_for_benchmark(int width, int freq_khz);
esp_err_t storage_service_unmount_for_benchmark(void);

#ifdef __cplusplus
}
#endif
