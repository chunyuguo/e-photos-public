#pragma once

#include "ephoto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void notification_service_publish(ephoto_notification_level_t level, const char *text, const char *screen_text);
void notification_service_clear(void);
void notification_service_snapshot(ephoto_notification_t *out_notification);

#ifdef __cplusplus
}
#endif
