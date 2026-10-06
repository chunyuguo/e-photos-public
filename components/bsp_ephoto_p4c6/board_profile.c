#include "board_profile.h"

#include <string.h>

#include "driver/gpio.h"

const board_profile_t *board_profile_get(void)
{
    static board_profile_t profile;
    static bool initialized = false;
    if (initialized) {
        return &profile;
    }

    memset(&profile, 0, sizeof(profile));
    profile.sdmmc_slot = 0;
    profile.sdmmc_width = 4;
    profile.sdmmc_internal_pullups = true;
    profile.sd_pwr_ldo_chan = 4;
    profile.sd_det = GPIO_NUM_45;
    profile.sd_det_active_low = true;
    profile.sd_det_pull_up = true;
    profile.sd_clk = GPIO_NUM_43;
    profile.sd_cmd = GPIO_NUM_44;
    profile.sd_d0 = GPIO_NUM_39;
    profile.sd_d1 = GPIO_NUM_40;
    profile.sd_d2 = GPIO_NUM_41;
    profile.sd_d3 = GPIO_NUM_42;
    profile.display_reset = GPIO_NUM_NC;
    profile.display_power = GPIO_NUM_NC;
    profile.backlight = GPIO_NUM_20;
    profile.ambient_light_gpio = GPIO_NUM_21;
    profile.rotation_switch_gpio = GPIO_NUM_34;
    profile.rotation_switch_active_low = true;
    profile.rotation_switch_pull_up = true;
    profile.backlight_pwm = true;
    profile.dsi_bus_id = 0;
    profile.dsi_num_data_lanes = 2;
    profile.dsi_lane_bit_rate_mbps = 1500;
    profile.dsi_phy_ldo_chan = 3;
    profile.dsi_phy_ldo_voltage_mv = 2500;
    profile.lcd_h_res = 800;
    profile.lcd_v_res = 1280;
    profile.default_rotation_deg = 90;
    profile.default_brightness = EPHOTO_BRIGHTNESS_MAX;
    strlcpy(profile.mount_path, "/sdcard", sizeof(profile.mount_path));
    strlcpy(profile.photo_dir, "/sdcard/photos", sizeof(profile.photo_dir));

    profile.buttons[EPHOTO_BUTTON_CONFIRM].gpio = GPIO_NUM_49;
    profile.buttons[EPHOTO_BUTTON_PREV].gpio = GPIO_NUM_50;
    profile.buttons[EPHOTO_BUTTON_NEXT].gpio = GPIO_NUM_51;
    profile.buttons[EPHOTO_BUTTON_ROTATE].gpio = GPIO_NUM_52;
    profile.buttons[EPHOTO_BUTTON_ZOOM].gpio = GPIO_NUM_53;

    for (size_t i = 0; i < EPHOTO_BUTTON_COUNT; ++i) {
        profile.buttons[i].active_level = false;
        profile.buttons[i].pull_up = true;
    }

    initialized = true;
    return &profile;
}
