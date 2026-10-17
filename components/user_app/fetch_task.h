#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "i2c_equipment.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the fetch module with RTC device
 * 
 * Must be called once before calling fetch_data_cycle().
 * 
 * @param rtc_dev Pointer to RTC device for time fetching
 */
void fetch_init(i2c_equipment *rtc_dev);

/**
 * @brief Perform one data fetch cycle synchronously
 * 
 * This function:
 * 1. Always updates time from RTC (no internet needed)
 * 2. Checks if internet operations are needed (market open or weather due)
 * 3. If internet not needed, returns immediately (fast path)
 * 4. If internet needed, fetches stock/weather/market data
 * 
 * The function is designed to be called synchronously from the main loop.
 * It will return quickly (~100-500ms) when no network operations are needed,
 * or take longer (5-15s) when fetching data over WiFi.
 * 
 * All fetched data is stored in the DataCache for UI consumption.
 */
void fetch_data_cycle(void);

/**
 * @brief Request WiFi connection for web server or other external needs
 * 
 * Sets a flag that forces the next fetch_data_cycle() to connect WiFi
 * regardless of market status. This is used when the web server is started
 * or other features need internet connectivity outside normal fetch schedule.
 * 
 * The flag is automatically cleared after the next successful WiFi connection.
 */
void fetch_request_wifi_connection(void);

/**
 * @brief Force the next fetch cycle to refresh weather immediately.
 */
void fetch_request_weather_refresh(void);

/**
 * @brief Keep the BSSID and channel of the connected access point in RTC memory.
 *
 * The next deep sleep wake connects to that access point directly, without a
 * channel scan. Call after any successful WiFi connection.
 */
void fetch_remember_wifi_ap(void);

#ifdef __cplusplus
}
#endif
