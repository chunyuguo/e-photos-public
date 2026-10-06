#include "app_controller.h"
#include "board_profile.h"
#include "ble_provisioning.h"
#include "boot_image_service.h"
#include "bsp_ephoto.h"
#include "clock_service.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "gallery_service.h"
#include "hosted_service.h"
#include "input_service.h"
#include "lan_discovery_service.h"
#include "nvs_flash.h"
#include "ota_service.h"
#include "settings_service.h"
#include "storage_service.h"
#include "web_api.h"
#include "wifi_admin.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ephoto_main";

static void init_nvs_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void snapshot_bridge(ephoto_app_state_t *out_state)
{
    app_controller_snapshot(out_state);
}

static void startup_gallery_scan_progress(void *context, size_t completed, size_t total)
{
    (void)context;
    display_service_set_startup_scan_progress(completed, total);
}

static void deferred_services_task(void *arg)
{
    (void)arg;

    int hosted_rc = esp_hosted_init();
    if (hosted_rc != ESP_OK) {
        ESP_LOGW(TAG, "deferred esp_hosted_init failed: %s", esp_err_to_name((esp_err_t)hosted_rc));
        ota_service_reject_boot_and_reboot();
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = wifi_admin_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deferred wifi init failed: %s", esp_err_to_name(err));
        ota_service_reject_boot_and_reboot();
        vTaskDelete(NULL);
        return;
    }

    ephoto_command_t refresh_wifi = {
        .type = EPHOTO_CMD_REFRESH_WIFI_STATE,
    };
    if (app_controller_submit(&refresh_wifi) != ESP_OK) {
        ESP_LOGW(TAG, "deferred wifi state refresh submit failed");
    }

    err = web_api_start(snapshot_bridge, app_controller_submit);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deferred web api start failed: %s", esp_err_to_name(err));
        ota_service_reject_boot_and_reboot();
        vTaskDelete(NULL);
        return;
    }

    err = lan_discovery_service_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lan discovery start failed: %s", esp_err_to_name(err));
        ota_service_reject_boot_and_reboot();
        vTaskDelete(NULL);
        return;
    }

    err = ble_provisioning_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ble provisioning start failed: %s", esp_err_to_name(err));
        ota_service_reject_boot_and_reboot();
        vTaskDelete(NULL);
        return;
    }

    // Keep a newly OTA-booted application pending long enough to catch
    // startup failures in the hosted C5/C6 and the public service layer.
    vTaskDelay(pdMS_TO_TICKS(10000));
    err = ota_service_confirm_boot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA boot confirmation deferred: %s", esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    init_nvs_or_recover();

    const board_profile_t *profile = board_profile_get();
    ESP_ERROR_CHECK(bsp_ephoto_init());
    ESP_ERROR_CHECK(settings_service_init(profile));
    ESP_ERROR_CHECK(boot_image_service_init());
    ESP_ERROR_CHECK(display_service_init(profile));
    esp_err_t err = storage_service_init(profile);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "storage init degraded: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(gallery_service_init(profile));
    err = gallery_service_rescan_with_progress(startup_gallery_scan_progress, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial gallery scan degraded: %s", esp_err_to_name(err));
    }
    err = hosted_service_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hosted diagnostics init degraded: %s", esp_err_to_name(err));
    }
    err = ota_service_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota service init degraded: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(app_controller_init(profile));
    ESP_ERROR_CHECK(app_controller_start());
    ESP_ERROR_CHECK(input_service_init(profile, app_controller_submit));
    xTaskCreatePinnedToCore(deferred_services_task,
                            "deferred_services",
                            6144,
                            NULL,
                            8,
                            NULL,
                            0);

    ESP_LOGI(TAG, "E-Photo app started");
}
