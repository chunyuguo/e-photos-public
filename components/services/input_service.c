#include "input_service.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    bool initialized;
    bool pressed;
    int stable_level;
    int last_sample_level;
    int64_t press_started_us;
    int64_t last_sample_change_us;
    int64_t last_event_us;
} button_runtime_t;

static const char *TAG = "input_service";
static const board_profile_t *s_profile;
static input_command_handler_t s_handler;
static button_runtime_t s_runtime[EPHOTO_BUTTON_COUNT];
static int64_t s_ignore_inputs_until_us;

#define BUTTON_SCAN_MS           5
#define BUTTON_DEBOUNCE_US       25000LL
#define BUTTON_MIN_PRESS_US      30000LL
#define BUTTON_EVENT_COOLDOWN_US 90000LL
#define BUTTON_BOOT_IGNORE_US    1500000LL
#define EPHOTO_CORE_INTERACTIVE  0
#define EPHOTO_PRIO_INPUT        12

static bool button_triggers_on_press(ephoto_button_id_t button)
{
    return button != EPHOTO_BUTTON_CONFIRM;
}

static ephoto_command_t build_command(ephoto_button_id_t button, int64_t duration_us)
{
    ephoto_command_t command = {
        .type = EPHOTO_CMD_NONE,
    };

    switch (button) {
    case EPHOTO_BUTTON_CONFIRM:
        command.type = duration_us >= 5000000LL ? EPHOTO_CMD_MENU_LONGPRESS : EPHOTO_CMD_HW_CONFIRM;
        break;
    case EPHOTO_BUTTON_PREV:
        command.type = EPHOTO_CMD_HW_PREV;
        break;
    case EPHOTO_BUTTON_NEXT:
        command.type = EPHOTO_CMD_HW_NEXT;
        break;
    case EPHOTO_BUTTON_ROTATE:
        command.type = EPHOTO_CMD_HW_ROTATE;
        break;
    case EPHOTO_BUTTON_ZOOM:
        command.type = EPHOTO_CMD_HW_ZOOM;
        break;
    default:
        break;
    }
    return command;
}

static void handle_button_transition(ephoto_button_id_t button, int level, int64_t now_us)
{
    button_runtime_t *runtime = &s_runtime[button];
    const ephoto_button_pin_t *pin = &s_profile->buttons[button];
    bool pressed_level = level == (pin->active_level ? 1 : 0);

    if (pressed_level) {
        if (now_us - runtime->last_event_us < BUTTON_EVENT_COOLDOWN_US) {
            return;
        }
        runtime->pressed = true;
        runtime->press_started_us = now_us;
        if (button_triggers_on_press(button)) {
            runtime->last_event_us = now_us;
            ephoto_command_t command = build_command(button, 0);
            if (command.type != EPHOTO_CMD_NONE && s_handler) {
                s_handler(&command);
            }
        }
        return;
    }

    if (!runtime->pressed) {
        return;
    }

    runtime->pressed = false;
    if (button_triggers_on_press(button)) {
        return;
    }

    int64_t press_duration_us = now_us - runtime->press_started_us;
    if (press_duration_us < BUTTON_MIN_PRESS_US) {
        return;
    }

    runtime->last_event_us = now_us;
    ephoto_command_t command = build_command(button, press_duration_us);
    if (command.type != EPHOTO_CMD_NONE && s_handler) {
        s_handler(&command);
    }
}

static void button_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "button_task started on CPU%d", (int)xPortGetCoreID());

    const TickType_t delay_ticks = pdMS_TO_TICKS(BUTTON_SCAN_MS);
    while (true) {
        int64_t now_us = esp_timer_get_time();
        if (now_us < s_ignore_inputs_until_us) {
            vTaskDelay(delay_ticks);
            continue;
        }

        for (size_t i = 0; i < EPHOTO_BUTTON_COUNT; ++i) {
            if (s_profile->buttons[i].gpio == GPIO_NUM_NC) {
                continue;
            }

            button_runtime_t *runtime = &s_runtime[i];
            int level = gpio_get_level(s_profile->buttons[i].gpio);

            if (!runtime->initialized) {
                runtime->initialized = true;
                runtime->stable_level = level;
                runtime->last_sample_level = level;
                runtime->last_sample_change_us = now_us;
                continue;
            }

            if (level != runtime->last_sample_level) {
                runtime->last_sample_level = level;
                runtime->last_sample_change_us = now_us;
                continue;
            }

            if (level == runtime->stable_level) {
                continue;
            }

            if (now_us - runtime->last_sample_change_us < BUTTON_DEBOUNCE_US) {
                continue;
            }

            runtime->stable_level = level;
            handle_button_transition((ephoto_button_id_t)i, level, now_us);
        }

        vTaskDelay(delay_ticks);
    }
}

esp_err_t input_service_init(const board_profile_t *profile, input_command_handler_t handler)
{
    s_profile = profile;
    s_handler = handler;
    memset(s_runtime, 0, sizeof(s_runtime));
    s_ignore_inputs_until_us = esp_timer_get_time() + BUTTON_BOOT_IGNORE_US;

    for (size_t i = 0; i < EPHOTO_BUTTON_COUNT; ++i) {
        if (profile->buttons[i].gpio == GPIO_NUM_NC) {
            continue;
        }
        gpio_config_t config = {
            .pin_bit_mask = 1ULL << profile->buttons[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = profile->buttons[i].pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "button gpio init failed");
    }

    xTaskCreatePinnedToCore(button_task,
                            "button_task",
                            4096,
                            NULL,
                            EPHOTO_PRIO_INPUT,
                            NULL,
                            EPHOTO_CORE_INTERACTIVE);
    ESP_LOGI(TAG, "input service ready");
    return ESP_OK;
}
