#include <stdint.h>

#include "board_profile.h"
#include "bsp_ephoto.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "storage_service.h"

static const char *TAG = "sd_bench_main";

typedef struct {
    const char *label;
    int width;
    int freq_khz;
} sd_bench_case_t;

static void init_nvs_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void run_sd_benchmark_case(const sd_bench_case_t *bench_case, int rounds)
{
    uint32_t write_sum = 0;
    uint32_t read_sum = 0;
    uint32_t write_min = UINT32_MAX;
    uint32_t write_max = 0;
    uint32_t read_min = UINT32_MAX;
    uint32_t read_max = 0;
    int passed_rounds = 0;

    ESP_LOGI(TAG,
             "SDTEST CASE START label=%s width=%d target_freq=%dkHz rounds=%d",
             bench_case->label,
             bench_case->width,
             bench_case->freq_khz,
             rounds);

    esp_err_t err = storage_service_remount_for_benchmark(bench_case->width, bench_case->freq_khz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "SDTEST CASE FAIL label=%s stage=mount err=%s",
                 bench_case->label,
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG,
             "SDTEST CASE MOUNTED label=%s width=%d real_freq=%dkHz",
             bench_case->label,
             storage_service_get_mounted_bus_width(),
             storage_service_get_mounted_freq_khz());

    for (int round = 1; round <= rounds; ++round) {
        ephoto_storage_benchmark_t result = {0};
        err = storage_service_run_benchmark(&result);
        if (err != ESP_OK || !result.ok) {
            ESP_LOGE(TAG,
                     "SDTEST ROUND FAIL label=%s round=%d err=%s",
                     bench_case->label,
                     round,
                     esp_err_to_name(err != ESP_OK ? err : result.error));
            break;
        }

        passed_rounds++;
        write_sum += result.write_kib_per_s;
        read_sum += result.read_kib_per_s;
        if (result.write_kib_per_s < write_min) {
            write_min = result.write_kib_per_s;
        }
        if (result.write_kib_per_s > write_max) {
            write_max = result.write_kib_per_s;
        }
        if (result.read_kib_per_s < read_min) {
            read_min = result.read_kib_per_s;
        }
        if (result.read_kib_per_s > read_max) {
            read_max = result.read_kib_per_s;
        }

        ESP_LOGI(TAG,
                 "SDTEST ROUND PASS label=%s round=%d write=%uKiB/s read=%uKiB/s write_ms=%lld read_ms=%lld",
                 bench_case->label,
                 round,
                 result.write_kib_per_s,
                 result.read_kib_per_s,
                 result.write_ms,
                 result.read_ms);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (passed_rounds == rounds) {
        ESP_LOGI(TAG,
                 "SDTEST CASE PASS label=%s rounds=%d avg_write=%uKiB/s avg_read=%uKiB/s min_write=%uKiB/s max_write=%uKiB/s min_read=%uKiB/s max_read=%uKiB/s",
                 bench_case->label,
                 rounds,
                 write_sum / (uint32_t)rounds,
                 read_sum / (uint32_t)rounds,
                 write_min,
                 write_max,
                 read_min,
                 read_max);
    } else {
        ESP_LOGW(TAG,
                 "SDTEST CASE UNSTABLE label=%s passed_rounds=%d/%d",
                 bench_case->label,
                 passed_rounds,
                 rounds);
    }
}

static void run_sd_benchmark_matrix(void)
{
    static const sd_bench_case_t cases[] = {
        {.label = "1bit-10MHz", .width = 1, .freq_khz = 10000},
        {.label = "1bit-20MHz", .width = 1, .freq_khz = 20000},
        {.label = "1bit-40MHz", .width = 1, .freq_khz = 40000},
        {.label = "4bit-10MHz", .width = 4, .freq_khz = 10000},
        {.label = "4bit-20MHz", .width = 4, .freq_khz = 20000},
        {.label = "4bit-40MHz", .width = 4, .freq_khz = 40000},
    };

    const int rounds = 3;
    ESP_LOGI(TAG,
             "SDTEST MATRIX START cases=%u rounds=%d bytes_per_round=%u",
             (unsigned)(sizeof(cases) / sizeof(cases[0])),
             rounds,
             4U * 1024U * 1024U);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        run_sd_benchmark_case(&cases[i], rounds);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    storage_service_unmount_for_benchmark();
    ESP_LOGI(TAG, "SDTEST MATRIX DONE");
}

void app_main(void)
{
    init_nvs_or_recover();

    const board_profile_t *profile = board_profile_get();
    ESP_ERROR_CHECK(bsp_ephoto_init());

    esp_err_t err = storage_service_init(profile);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDTEST initial storage init failed: %s", esp_err_to_name(err));
    } else {
        run_sd_benchmark_matrix();
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
