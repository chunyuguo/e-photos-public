#include "ambient_light_service.h"

#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ambient_light";
// Calibrate the photoresistor range to the board's real-world ADC span so
// auto-brightness can still reach the full UI range.
static const uint16_t AMBIENT_LIGHT_EFFECTIVE_MIN_MV = 528;   // about 16%
static const uint16_t AMBIENT_LIGHT_EFFECTIVE_MAX_MV = 3102;  // about 94%
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static adc_channel_t s_channel;
static bool s_available;
static bool s_cali_ready;
static ambient_light_status_t s_status;

static uint8_t ambient_light_percent_from_mv(uint16_t millivolts)
{
    if (millivolts <= AMBIENT_LIGHT_EFFECTIVE_MIN_MV) {
        return 0;
    }
    if (millivolts >= AMBIENT_LIGHT_EFFECTIVE_MAX_MV) {
        return 100;
    }

    uint32_t numerator = (uint32_t)(millivolts - AMBIENT_LIGHT_EFFECTIVE_MIN_MV) * 100U;
    uint32_t denominator = (uint32_t)(AMBIENT_LIGHT_EFFECTIVE_MAX_MV - AMBIENT_LIGHT_EFFECTIVE_MIN_MV);
    return (uint8_t)((numerator + denominator / 2U) / denominator);
}

static esp_err_t init_adc_calibration(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle);
#else
    (void)unit;
    (void)channel;
    (void)atten;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t ambient_light_service_init(const board_profile_t *profile)
{
    if (!profile || profile->ambient_light_gpio == GPIO_NUM_NC) {
        memset(&s_status, 0, sizeof(s_status));
        s_available = false;
        return ESP_OK;
    }

    adc_unit_t unit = ADC_UNIT_1;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(profile->ambient_light_gpio, &unit, &s_channel),
                        TAG,
                        "ambient light gpio is not ADC capable");

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle), TAG, "create ambient light adc failed");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, s_channel, &chan_cfg),
                        TAG,
                        "config ambient light adc channel failed");

    if (init_adc_calibration(unit, s_channel, chan_cfg.atten) == ESP_OK) {
        s_cali_ready = true;
    } else {
        s_cali_ready = false;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.available = true;
    s_available = true;
    ESP_LOGI(TAG,
             "ambient light input ready on GPIO%d (ADC channel %d, calibration=%s)",
             (int)profile->ambient_light_gpio,
             (int)s_channel,
             s_cali_ready ? "on" : "off");
    return ESP_OK;
}

esp_err_t ambient_light_service_poll(ambient_light_status_t *out_status)
{
    if (!s_available || !s_adc_handle) {
        if (out_status) {
            memset(out_status, 0, sizeof(*out_status));
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    int mv_sum = 0;
    int raw = 0;
    int mv = 0;
    int samples = 0;

    for (int i = 0; i < 4; ++i) {
        if (adc_oneshot_read(s_adc_handle, s_channel, &raw) != ESP_OK) {
            continue;
        }
        samples += 1;
        if (s_cali_ready && adc_oneshot_get_calibrated_result(s_adc_handle, s_cali_handle, s_channel, &mv) == ESP_OK) {
            mv_sum += mv;
        } else {
            mv_sum += (raw * 3300) / 4095;
        }
    }

    if (samples <= 0) {
        if (out_status) {
            *out_status = s_status;
        }
        return ESP_ERR_TIMEOUT;
    }

    uint16_t averaged_mv = (uint16_t)(mv_sum / samples);
    uint8_t percent = ambient_light_percent_from_mv(averaged_mv);

    bool had_valid_status = s_status.valid;
    s_status.available = true;
    s_status.valid = true;
    s_status.millivolts = had_valid_status ? (uint16_t)((s_status.millivolts * 3U + averaged_mv) / 4U) : averaged_mv;
    s_status.percent = had_valid_status ? (uint8_t)((s_status.percent * 3U + percent) / 4U) : percent;

    if (out_status) {
        *out_status = s_status;
    }
    return ESP_OK;
}

void ambient_light_service_get_status(ambient_light_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    *out_status = s_status;
}
