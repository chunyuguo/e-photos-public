#include "factory_reset_service.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot_image_service.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gallery_service.h"
#include "nvs.h"
#include "notification_service.h"
#include "storage_service.h"

static const char *TAG = "factory_reset";

static SemaphoreHandle_t s_mutex;
static bool s_running;

static void ensure_mutex(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

static bool mark_running(bool running)
{
    ensure_mutex();
    if (!s_mutex) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (running && s_running) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_running = running;
    xSemaphoreGive(s_mutex);
    return true;
}

bool factory_reset_service_is_running(void)
{
    bool running = false;
    ensure_mutex();
    if (!s_mutex) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    running = s_running;
    xSemaphoreGive(s_mutex);
    return running;
}

static esp_err_t erase_nvs_namespace(const char *ns_name)
{
    if (!ns_name || !ns_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ns_name, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open namespace failed");

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t remove_tree_contents(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (errno == ENOENT) {
            return ESP_OK;
        }
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
        if (snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name) <= 0) {
            closedir(dir);
            return ESP_ERR_INVALID_SIZE;
        }

        struct stat st = {0};
        if (stat(child_path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            ESP_RETURN_ON_ERROR(remove_tree_contents(child_path), TAG, "clear dir %s failed", child_path);
            if (rmdir(child_path) != 0 && errno != ENOENT) {
                closedir(dir);
                return ESP_FAIL;
            }
        } else if (unlink(child_path) != 0 && errno != ENOENT) {
            closedir(dir);
            return ESP_FAIL;
        }
    }

    closedir(dir);
    return ESP_OK;
}

static esp_err_t ensure_dir_exists(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t reset_storage_layout(void)
{
    const char *mount_path = storage_service_get_mount_path();
    const char *photo_dir = storage_service_get_photo_dir();
    char meta_dir[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    char cache_dir[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};

    if (!mount_path || !mount_path[0] || !photo_dir || !photo_dir[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    if (snprintf(meta_dir, sizeof(meta_dir), "%s/.ephoto_meta", mount_path) <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (snprintf(cache_dir, sizeof(cache_dir), "%s/.ephoto_cache", mount_path) <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(remove_tree_contents(mount_path), TAG, "clear storage root failed");

    ESP_RETURN_ON_ERROR(ensure_dir_exists(photo_dir), TAG, "ensure photo dir failed");
    ESP_RETURN_ON_ERROR(ensure_dir_exists(meta_dir), TAG, "ensure meta dir failed");
    ESP_RETURN_ON_ERROR(ensure_dir_exists(cache_dir), TAG, "ensure cache dir failed");
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

void factory_reset_service_schedule_reboot(uint32_t delay_ms)
{
    uint32_t safe_delay_ms = delay_ms < 800 ? 800 : delay_ms;
    xTaskCreatePinnedToCore(reboot_task,
                            "factory_reset_reboot",
                            3072,
                            (void *)(uintptr_t)safe_delay_ms,
                            4,
                            NULL,
                            0);
}

esp_err_t factory_reset_service_run(void)
{
    if (!mark_running(true)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    notification_service_publish(EPHOTO_NOTIFICATION_INFO,
                                 "正在恢复出厂设置",
                                 "恢复出厂设置中");

    err = display_service_purge_all_caches();
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        goto done;
    }

    err = reset_storage_layout();
    if (err != ESP_OK) {
        goto done;
    }

    err = gallery_service_rescan();
    if (err != ESP_OK) {
        goto done;
    }

    err = erase_nvs_namespace("settings");
    if (err != ESP_OK) {
        goto done;
    }
    err = erase_nvs_namespace("wifi_admin");
    if (err != ESP_OK) {
        goto done;
    }
    err = erase_nvs_namespace("ota");
    if (err != ESP_OK) {
        goto done;
    }

    err = boot_image_service_factory_reset();
    if (err != ESP_OK) {
        goto done;
    }

    notification_service_publish(EPHOTO_NOTIFICATION_SUCCESS,
                                 "恢复出厂设置完成，即将重启",
                                 "恢复完成，即将重启");

done:
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory reset failed: %s", esp_err_to_name(err));
        notification_service_publish(EPHOTO_NOTIFICATION_ERROR,
                                     "恢复出厂设置失败，请重试",
                                     "恢复失败");
    }
    mark_running(false);
    return err;
}
