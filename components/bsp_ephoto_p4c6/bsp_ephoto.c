#include "bsp_ephoto.h"

#include "board_profile.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "bsp_ephoto";

static esp_err_t configure_optional_output(gpio_num_t gpio, int initial_level)
{
    if (gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "gpio config failed");
    return gpio_set_level(gpio, initial_level ? 1 : 0);
}

static esp_err_t configure_optional_input(gpio_num_t gpio, bool pull_up)
{
    if (gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

esp_err_t bsp_ephoto_init(void)
{
    const board_profile_t *profile = board_profile_get();

    ESP_RETURN_ON_ERROR(configure_optional_output(profile->display_power, 0), TAG, "display power gpio init failed");
    ESP_RETURN_ON_ERROR(configure_optional_output(profile->display_reset, 0), TAG, "display reset gpio init failed");
    ESP_RETURN_ON_ERROR(configure_optional_output(profile->backlight, 0), TAG, "backlight gpio init failed");
    ESP_RETURN_ON_ERROR(configure_optional_input(profile->sd_det, profile->sd_det_pull_up), TAG, "sd detect gpio init failed");
    ESP_RETURN_ON_ERROR(configure_optional_input(profile->rotation_switch_gpio, profile->rotation_switch_pull_up), TAG, "rotation switch gpio init failed");

    ESP_LOGI(TAG,
             "Board profile ready: sdmmc slot=%d width=%d mount=%s photo_dir=%s",
             profile->sdmmc_slot,
             profile->sdmmc_width,
             profile->mount_path,
             profile->photo_dir);
    return ESP_OK;
}
