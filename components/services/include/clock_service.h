#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool auto_sync_enabled;
    bool sync_in_progress;
    bool time_valid;
    int64_t last_sync_ms;
    char last_sync_time[32];
    char last_sync_source[16];
    char timezone[32];
} clock_service_status_t;

void clock_service_init(const char *timezone, bool auto_sync_enabled);
void clock_service_set_timezone(const char *timezone);
void clock_service_set_auto_sync_enabled(bool enabled);
void clock_service_poll(bool network_ready);
esp_err_t clock_service_sync_now(void);
esp_err_t clock_service_set_manual_time_string(const char *local_datetime);
void clock_service_get_status(clock_service_status_t *status);
void clock_service_get_time_string(char *buffer, size_t buffer_len);
bool clock_service_get_local_tm(struct tm *out_tm);
int64_t clock_service_now_ms(void);
int64_t clock_service_wall_time_ms(void);

#ifdef __cplusplus
}
#endif
