#include "notification_service.h"

#include <string.h>

#include "clock_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mutex;
static ephoto_notification_t s_notification;

static void ensure_ready(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

void notification_service_publish(ephoto_notification_level_t level, const char *text, const char *screen_text)
{
    ensure_ready();
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_notification.level = level;
    strlcpy(s_notification.text, text ? text : "", sizeof(s_notification.text));
    strlcpy(s_notification.screen_text, screen_text ? screen_text : "", sizeof(s_notification.screen_text));
    s_notification.updated_at_ms = clock_service_now_ms();
    xSemaphoreGive(s_mutex);
}

void notification_service_clear(void)
{
    ensure_ready();
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_notification, 0, sizeof(s_notification));
    xSemaphoreGive(s_mutex);
}

void notification_service_snapshot(ephoto_notification_t *out_notification)
{
    ensure_ready();
    if (!s_mutex) {
        memset(out_notification, 0, sizeof(*out_notification));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out_notification = s_notification;
    xSemaphoreGive(s_mutex);
}
