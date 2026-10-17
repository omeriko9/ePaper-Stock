#ifndef USER_APP_H
#define USER_APP_H

#include "esp_system.h"

// EPD access now routed via EPaper singleton
class EPaper;

#ifdef __cplusplus
extern "C" {
#endif

void user_app_init(esp_reset_reason_t reset_reason);
void user_ui_init(void);

// Hooks for web server/actions
void user_request_refresh(void);
void user_set_device_sleep_enabled(bool enabled);
void user_set_dead_battery_image_test_mode(bool enabled);
bool user_get_dead_battery_image_test_mode(void);
void user_abort_main_delay(void);

// Sync the clock from NTP if the last sync is older than a day. WiFi must be up.
// Waits up to wait_ms for the answer.
void user_ntp_sync_if_due(uint32_t wait_ms);

#ifdef __cplusplus
}
#endif

#endif
