// user_app.cpp (refactored with proper sleep chunking and FSM)
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "user_app.h"
#include "user_config.h"
#include "utilities.h"
#include <esp_sntp.h>
#include "i2c_equipment.h"
#include <esp_pm.h>
#include "esp_wifi.h"
#include <nvs_flash.h>
#include <nvs.h>

#include "button_bsp.h"
#include <WiFiManager.h>
#include "settings.h"
#include "web_server.h"
#include <WiFi.h>
#include "ePaper.h"
#include "data_cache.h"
#include "fetch_task.h"
#include "audio_player.h"
#include "esp_netif.h"
#include <algorithm>
#include "esp_attr.h"
#include "log_capture.h"
#include "power_mgmt.h"

static const char *TAG = "user_app";
static const char *APP_TAG = "APP";
static const char *DEAD_BATTERY_IMAGE_PATH = "/spiffs/dead_battery_image.bin";
static constexpr uint8_t DEAD_BATTERY_IMAGE_THRESHOLD_PCT = 20;
static constexpr size_t DEAD_BATTERY_IMAGE_BYTES = (EPD_WIDTH * EPD_HEIGHT) / 8;

// Forward declaration
static void process_deferred_button_wake(void);
extern "C" void user_abort_main_delay(void);

// RTC retained sleep state (survives deep sleep resets)
RTC_DATA_ATTR static uint8_t s_rtc_next_stock_index = 0;
RTC_DATA_ATTR static uint32_t s_rtc_expected_sleep_ms = 0;
RTC_DATA_ATTR static bool s_rtc_cache_preserved = false;
RTC_DATA_ATTR static uint32_t s_last_ntp_sync_time = 0;
RTC_DATA_ATTR static uint64_t s_button_wakeup_pin_mask = 0;

// Boot-scoped state
static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;
static esp_sleep_wakeup_cause_t s_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static bool s_cached_data_ready_on_boot = false;

// Hardware and system globals
WiFiManager wm;
i2c_equipment *rtc_dev = NULL;

// Task handles
static TaskHandle_t wifi_ap_msg_handle = NULL;
static TaskHandle_t web_server_ip_display_handle = NULL;
static TaskHandle_t loop_direct_ft_task_handle = NULL;

// Synchronization
static SemaphoreHandle_t s_epd_mutex = nullptr;

// Application state flags
bool time_synced = false;
static volatile bool s_web_server_manual_control = false;
static volatile bool s_skip_next_display_update = false;  // Skip display update after showing IP
static volatile bool s_dead_battery_image_test_mode = false;
static QueueHandle_t s_button_event_queue = nullptr;

// Captive portal UI state (rendered by main loop only)
// No captive-portal global UI flags; render via callback task

// Constants
#define GREEN_LED_PIN GPIO_NUM_3
#define BUTTON_SETTLE_TIMEOUT_MS 400 // Max time to wait for button multi-click detection

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
static void handle_button_event(button_event_t evt);
static void wait_for_button_state_machine_idle(void);
static void display_update_cycle(void);
static void sleep_cycle(void);
static uint32_t compute_sleep_window_ms(void);
static void prepare_for_deep_sleep(uint32_t sleep_ms);
static bool apply_boot_wakeup_policy(void);
static void awake_idle_wait(uint32_t ms);
static void display_web_server_ip(const char *audio_file = nullptr);
static bool render_dead_battery_image_if_needed(uint8_t battery_percent);
static void render_display_content(const stock_data_t &stock, const weather_data_t &weather,
                                   const time_data_t &t, uint8_t bat, bool market_open,
                                   const char *current_symbol);

static void render_captive_portal_screen(const char *ssid)
{
    // Assumes s_epd_mutex is already held by the caller
    EPaper &ep = EPaper::get_instance();
    ep.clear_buffer();
    // Use built-in 5x7 font to avoid FreeType usage from callback context
    const char *line1 = "Connect to WiFi";
    const char *line2 = (ssid && *ssid) ? ssid : "ePaper-Config";
    const char *line3 = "Open 192.168.4.1";

    // Simple vertical layout
    int y1 = 30;
    int y2 = 90;
    int y3 = 150;
    ep.draw_text5x7_center(y1, line1, 2);
    ep.draw_text5x7_center(y2, line2, 2);
    ep.draw_text5x7_center(y3, line3, 2);
    ep.display_full_refresh();
}

// Background task: consume button events and run unified handler
static void button_event_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Button event task started");
    button_event_t evt;
    for (;;)
    {
        if (xQueueReceive(s_button_event_queue, &evt, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGI(TAG, "Button event received: %d", (int)evt);
            // Suspend main loop while handling user interaction
            if (loop_direct_ft_task_handle != NULL)
            {
                ESP_LOGD(TAG, "Suspending main loop task for button handling");
                vTaskSuspend(loop_direct_ft_task_handle);
            }

            // Process this event and drain any burst
            handle_button_event(evt);
            int burst_count = 0;
            while (xQueueReceive(s_button_event_queue, &evt, 0) == pdTRUE)
            {
                handle_button_event(evt);
                burst_count++;
            }
            if (burst_count > 0)
            {
                ESP_LOGI(TAG, "Processed %d additional button events in burst", burst_count);
            }

            // Resume main loop
            if (loop_direct_ft_task_handle != NULL)
            {
                ESP_LOGD(TAG, "Resuming main loop task after button handling");
                vTaskResume(loop_direct_ft_task_handle);
            }
        }
    }
}

// ============================================================================
// BUTTON EVENT HANDLING
// ============================================================================

/**
 * @brief Wait for button state machine to complete multi-click detection.
 *
 * The button library uses a timer-based state machine that needs continuous
 * execution to detect double-clicks (waits ~300ms for second click).
 * This function blocks until both buttons are in IDLE state or timeout occurs.
 */
static void wait_for_button_state_machine_idle(void)
{
    uint32_t elapsed_ms = 0;
    while (!user_buttons_are_idle() && elapsed_ms < BUTTON_SETTLE_TIMEOUT_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed_ms += 10;
    }

    if (elapsed_ms > 0)
    {
        ESP_LOGD(APP_TAG, "Waited %ums for button state machine to settle", elapsed_ms);
    }
}

/**
 * @brief Process all pending button events.
 *
 * Called from main task context. Safe to call audio_player and perform
 * heavy operations. Uses atomic xEventGroupWaitBits to fetch and clear
 * all pending events in one operation.
 */
static void handle_button_event(button_event_t evt)
{
    switch (evt)
    {
    case BTN_EVT_BOOT_SINGLE:
    {
        ESP_LOGI(TAG, "BOOT single: toggle web server");
        bool was_running = web_server_is_running();
        if (was_running)
        {
            web_server_stop();
            s_web_server_manual_control = false;
            audio_player_play_nonblocking("server_down.mp3");
            vTaskDelay(pdMS_TO_TICKS(500)); // Wait for sound to start
            esp_restart();            
        }
        else
        {
            // Start web server FIRST so the flag is set
            ESP_LOGI(TAG, "Starting web server...");
            if (web_server_start())
            {
                ESP_LOGI(TAG, "Web server started successfully");
                s_web_server_manual_control = true;

                // Request WiFi connection for web server
                // This sets a flag that tells fetch_data_cycle to connect WiFi
                ESP_LOGI(TAG, "Requesting WiFi connection for web server...");
                fetch_request_wifi_connection();
                
                // Wake up main loop immediately so it can connect WiFi
                // The main loop will call fetch_data_cycle() which will see the request flag
                ESP_LOGI(TAG, "Waking main loop to establish WiFi connection...");
                user_abort_main_delay();
                
                // Give the main loop time to run fetch and connect WiFi
                // We're running in button_event_task context, main loop is suspended by caller
                // Resume main loop temporarily to let it connect WiFi
                if (loop_direct_ft_task_handle != NULL)
                {
                    ESP_LOGI(TAG, "Resuming main loop to connect WiFi...");
                    vTaskResume(loop_direct_ft_task_handle);
                    
                    // Wait for WiFi to connect (up to 15 seconds)
                    ESP_LOGI(TAG, "Waiting for WiFi connection (max 15s)...");
                    int wait_count = 0;
                    while (!WiFi.isConnected() && wait_count < 150)
                    {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        wait_count++;
                        if (wait_count % 10 == 0)
                        {
                            ESP_LOGI(TAG, "Still waiting for WiFi... (%ds)", wait_count / 10);
                        }
                    }
                    
                    // Suspend main loop again so we can proceed with web server startup
                    ESP_LOGI(TAG, "Suspending main loop again after WiFi wait");
                    vTaskSuspend(loop_direct_ft_task_handle);
                    
                    if (WiFi.isConnected())
                    {
                        ESP_LOGI(TAG, "WiFi connected successfully after %dms", wait_count * 100);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "WiFi failed to connect after %dms - will try to start server anyway", wait_count * 100);
                    }
                }
                
                // Play sound first, then show IP
                audio_player_play_nonblocking("server_up.mp3");
                display_web_server_ip(nullptr);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to start web server");
                audio_player_play_nonblocking("error.mp3");
            }
            ESP_LOGI(TAG, "Web server button handler complete");
        }
        break;
    }
    case BTN_EVT_BOOT_LONG:
    {
        ESP_LOGI(TAG, "BOOT long: entering deep sleep");
        AudioPlayer::instance().play("bye.mp3", true);
        AudioPlayer::instance().deinit();
        power_enter_deep_sleep(0); // wake only on the BOOT button
        break;
    }
    case BTN_EVT_BOOT_DOUBLE:
        ESP_LOGI(TAG, "BOOT double: ting");
        audio_player_play_nonblocking("ting.mp3");
        esp_restart();
        break;
    case BTN_EVT_PWR_SINGLE:
        ESP_LOGI(TAG, "PWR single: pop");
        audio_player_play_nonblocking("pop.mp3");
        break;
    case BTN_EVT_PWR_LONG:
        ESP_LOGI(TAG, "PWR long: whoosh");
        audio_player_play_nonblocking("whoosh.mp3");
        break;
    case BTN_EVT_PWR_DOUBLE:
        ESP_LOGI(TAG, "PWR double: ting");
        audio_player_play_nonblocking("ting.mp3");
        break;
    default:
        break;
    }
}

// ============================================================================
// POWER MANAGEMENT - AWAKE IDLE WAIT
// ============================================================================

static void awake_idle_wait(uint32_t ms)
{
    uint32_t notification_value = 0;
    (void)xTaskNotifyWait(0x00, 0xFFFFFFFF, &notification_value, pdMS_TO_TICKS(ms));
    wait_for_button_state_machine_idle();
}

// ============================================================================
// DEEP SLEEP PREPARATION
// ============================================================================

static uint32_t compute_sleep_window_ms(void)
{
    // Check if market is open or if mock mode is enabled
    bool market_open = DataCache::instance().get_market_open();
    bool mock_mode = Settings::instance().mock_mode_enabled();
    
    // Use market open sleep interval if market is open OR mock mode is enabled
    // Use market closed sleep interval otherwise
    if (market_open || mock_mode) {
        return Settings::instance().sleep_interval_ms();
    } else {
        return Settings::instance().market_closed_sleep_ms();
    }
}

static void prepare_for_deep_sleep(uint32_t sleep_ms)
{
    uint8_t stock_count = Settings::instance().stock_count();
    if (stock_count == 0)
    {
        s_rtc_cache_preserved = false;
        s_rtc_expected_sleep_ms = 0;
        return;
    }

    uint8_t idx = DataCache::instance().get_current_stock_index();
    s_rtc_next_stock_index = idx;
    s_rtc_expected_sleep_ms = sleep_ms;
    s_rtc_cache_preserved = true;

    ESP_LOGI(APP_TAG, "Deep sleep prep: stock index %u, sleep %u ms", idx, sleep_ms);
}

/**
 * @brief Main sleep cycle - handles both chunked sleep and delay mode.
 *
 * Determines sleep duration from settings, prepares system, sleeps, and restores state.
 */
static void sleep_cycle(void)
{
    const uint32_t ms = compute_sleep_window_ms();

    ESP_LOGI(APP_TAG, "Sleep cycle: %u ms", ms);

    prepare_for_deep_sleep(ms);

    // Let a sound that is still playing (e.g. a price alert) finish.
    for (int i = 0; i < 50 && AudioPlayer::instance().is_playing(); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // The audio rail stays on in deep sleep, so put the codec into standby.
    AudioPlayer::instance().deinit();

    power_enter_deep_sleep((uint64_t)ms * 1000ULL);
}

// ============================================================================
// DISPLAY UPDATE CYCLE
// ============================================================================

static void render_display_content(const stock_data_t &stock, const weather_data_t &weather,
                                   const time_data_t &t, uint8_t bat, bool market_open,
                                   const char *current_symbol)
{
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", t.hour, t.minute);
    ESP_LOGI(APP_TAG, "About to be displayed time=%s", time_str);
    
    // Log premarket status for debugging
    ESP_LOGI(APP_TAG, "[DISPLAY] stock.valid=%d, stock.has_premarket=%d, market_open=%d",
             stock.valid, stock.has_premarket, market_open);
    if (stock.valid && stock.has_premarket) {
        ESP_LOGI(APP_TAG, "[DISPLAY] Premarket data: price=%s, change=%s",
                 stock.premarket_price_str, stock.premarket_change_percent_str);
    }
    
    char batt_str[16];
    snprintf(batt_str, sizeof(batt_str), "%u%%", (unsigned)bat);
    bool show_server = web_server_is_running();
    EPaper::get_instance().print_status(time_str, batt_str, bat, show_server);

    char big[32];
    char footer[128];

    if (market_open)
    {
        if (stock.valid)
            snprintf(big, sizeof(big), "%s", stock.current_price_str);
        else
            snprintf(big, sizeof(big), "--");

        EPaper::get_instance().print_center(big);

        char cd_close[32];
        Utilities::format_countdown_to_close(cd_close, sizeof(cd_close));
        EPaper::get_instance().print_below_center(cd_close);

        EPaper::get_instance().print_title(current_symbol);

        if (stock.valid)
        {
            if (stock.has_premarket)
            {
                snprintf(footer, sizeof(footer), "%s | Pre: %s (%s)",
                         stock.change_percent_str,
                         stock.premarket_price_str,
                         stock.premarket_change_percent_str);
            }
            else
            {
                snprintf(footer, sizeof(footer), "%s | Prev: %s",
                         stock.change_percent_str,
                         stock.previous_close_str);
            }
        }
        else
        {
            snprintf(footer, sizeof(footer), "No data");
        }
        EPaper::get_instance().print_footer(footer);
    }
    else
    {
        if (weather.valid)
        {
            snprintf(big, sizeof(big), "%s", weather.temperature);
            EPaper::get_instance().print_center(big);

            char cd[32];
            Utilities::format_countdown_to_open(cd, sizeof(cd));
            EPaper::get_instance().print_below_center(cd);

            EPaper::get_instance().print_title(weather.condition);

            if (stock.valid)
            {
                if (stock.has_premarket)
                {
                    ESP_LOGI(APP_TAG, "[DISPLAY] Market closed - showing PREMARKET price: %s (%s) for %s",
                             stock.premarket_price_str, stock.premarket_change_percent_str, current_symbol);
                    EPaper::get_instance().print_above_footer(stock.premarket_price_str, stock.premarket_change_percent_str, current_symbol);
                }
                else
                {
                    ESP_LOGI(APP_TAG, "[DISPLAY] Market closed - showing LAST price: %s (%s) for %s",
                             stock.current_price_str, stock.change_percent_str, current_symbol);
                    EPaper::get_instance().print_above_footer(stock.current_price_str, stock.change_percent_str, current_symbol);
                }
            }

            snprintf(footer, sizeof(footer), "Hum: %s | Rain: %s", weather.humidity, weather.rain_mm);
            EPaper::get_instance().print_footer(footer);
        }
        else
        {
            EPaper::get_instance().print_center("--");
            EPaper::get_instance().print_title("Weather");
            EPaper::get_instance().print_footer("No data");
        }
    }
}

static bool render_dead_battery_image_if_needed(uint8_t battery_percent)
{
    bool should_show = s_dead_battery_image_test_mode ||
                       (Settings::instance().dead_battery_image_enabled() && battery_percent < DEAD_BATTERY_IMAGE_THRESHOLD_PCT);
    if (!should_show)
    {
        return false;
    }

    FILE *image_file = fopen(DEAD_BATTERY_IMAGE_PATH, "rb");
    if (!image_file)
    {
        ESP_LOGW(APP_TAG, "Dead battery image requested but file is missing");
        return false;
    }

    uint8_t *bitmap = static_cast<uint8_t *>(heap_caps_malloc(DEAD_BATTERY_IMAGE_BYTES, MALLOC_CAP_8BIT));
    if (!bitmap)
    {
        fclose(image_file);
        ESP_LOGE(APP_TAG, "Failed to allocate dead battery image buffer");
        return false;
    }

    size_t bytes_read = fread(bitmap, 1, DEAD_BATTERY_IMAGE_BYTES, image_file);
    fclose(image_file);
    if (bytes_read != DEAD_BATTERY_IMAGE_BYTES)
    {
        free(bitmap);
        ESP_LOGE(APP_TAG, "Dead battery image has invalid size: %u", (unsigned)bytes_read);
        return false;
    }

    EPaper::get_instance().draw_packed_bitmap_1bit(bitmap, DEAD_BATTERY_IMAGE_BYTES);
    free(bitmap);
    ESP_LOGI(APP_TAG, "Displayed dead battery image (%s)",
             s_dead_battery_image_test_mode ? "test mode" : "battery threshold");
    return true;
}

static void display_update_cycle(void)
{
    // Skip display update if IP is currently being shown
    if (s_skip_next_display_update) {
        ESP_LOGI(APP_TAG, "Skipping display update - showing web server IP");
        return;
    }

    ESP_LOGI(APP_TAG, "Free heap: %d bytes, Min free heap: %d bytes",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    gpio_set_level((gpio_num_t)GREEN_LED_PIN, 0);

    uint8_t current_stock_idx = DataCache::instance().get_current_stock_index();
    stock_data_t stock = DataCache::instance().get_stock_data(current_stock_idx);
    weather_data_t weather = DataCache::instance().get_weather_current_data();
    time_data_t t = DataCache::instance().get_time_data();
    uint8_t bat = DataCache::instance().get_battery_level();
    bool market_open = DataCache::instance().get_market_open();
    uint8_t stock_count = Settings::instance().stock_count();
    const StockEntry *stocks = Settings::instance().stocks();

    // In mock mode, force market to appear open to display stock data instead of weather
    if (Settings::instance().mock_mode_enabled())
    {
        market_open = true;
    }

    const char *current_symbol = (stock_count > 0 && current_stock_idx < stock_count)
                                     ? stocks[current_stock_idx].symbol
                                     : "---";

    ESP_LOGI(APP_TAG, "Market: %s | Stock[%u/%u] %s valid: %s | Weather valid: %s | Time: %02d:%02d | Bat: %d%%",
             market_open ? "OPEN" : "CLOSED",
             current_stock_idx, stock_count, current_symbol,
             stock.valid ? "YES" : "NO",
             weather.valid ? "YES" : "NO",
             t.hour, t.minute, bat);

    // SIMPLIFIED DISPLAY UPDATE:
    // Always update display on every wakeup cycle (no dirty checking)
    
    if (xSemaphoreTake(s_epd_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGW(APP_TAG, "Failed to acquire EPD mutex, skipping update");
        return;
    }

    EPaper::get_instance().clear_buffer();
    if (!render_dead_battery_image_if_needed(bat))
    {
        render_display_content(stock, weather, t, bat, market_open, current_symbol);
    }

    // Partial refresh against the stored previous frame; EPaper does a full refresh
    // when needed (first image after reset, and every few updates against ghosting).
    EPaper::get_instance().present(true);
    xSemaphoreGive(s_epd_mutex);

    awake_idle_wait(Settings::instance().awake_after_update_ms());

    gpio_set_level((gpio_num_t)GREEN_LED_PIN, 1);
}

// ============================================================================
// MAIN LOOP TASK
// ============================================================================

/**
 * @brief Main application loop task.
 *
 * State machine:
 * 1. Check for button events
 * 2. Update display (if data dirty or force refresh)
 * 3. Check for force refresh (skip sleep if set)
 * 4. Sleep cycle
 * 5. Process post-wake button events
 * 6. Repeat
 */
static void loop_direct_ft(void *arg)
{
    (void)arg;
    ESP_LOGI(APP_TAG, "Starting main loop");

    // Initialize display once (but skip if showing IP - IP task will handle display)
    if (!s_skip_next_display_update) {
        ESP_LOGI(APP_TAG, "Initializing EPD");
        EPaper::get_instance().init();
    } else {
        ESP_LOGI(APP_TAG, "Skipping EPD init - IP display task will use EPD");
    }

    // Main loop
    for (;;)
    {
        // SYNCHRONOUS FETCH: Call fetch directly at start of each cycle
        // This updates RTC time first, then checks if network operations needed
        // Returns quickly (~100-500ms) when no internet needed
        ESP_LOGI(APP_TAG, "Calling fetch_data_cycle...");
        fetch_data_cycle();

        display_update_cycle();

        const uint32_t ms = compute_sleep_window_ms();

        // Don't sleep if web server is manually started
        if (s_web_server_manual_control)
        {
            ESP_LOGI(APP_TAG, "Web server active - staying awake");
            vTaskDelay(pdMS_TO_TICKS(ms)); 
            continue;
        }

        uint8_t stock_count = Settings::instance().stock_count();
        if (stock_count > 1)
        {
            DataCache::instance().advance_stock_index();
            ESP_LOGI(APP_TAG, "Advanced to stock index %u for next cycle",
                     DataCache::instance().get_current_stock_index());
        }

        sleep_cycle();
    }
}

// ============================================================================
// INITIALIZATION FUNCTIONS
// ============================================================================

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(APP_TAG, "Time synchronized from NTP server!");
    time_synced = true;
    s_last_ntp_sync_time = (uint32_t)time(NULL);

    time_t system_time_utc = time(NULL); // System time is always UTC
    struct tm utc_tm = {0};
    gmtime_r(&system_time_utc, &utc_tm); // Convert to UTC tm struct

    ESP_LOGI(APP_TAG, "NTP Time (UTC): %04d-%02d-%02d %02d:%02d:%02d",
             utc_tm.tm_year + 1900,
             utc_tm.tm_mon + 1,
             utc_tm.tm_mday,
             utc_tm.tm_hour,
             utc_tm.tm_min,
             utc_tm.tm_sec);

    // Write UTC time to RTC chip (RTC always stores UTC)
    if (rtc_dev != NULL)
    {
        rtc_dev->set_rtcTime(
            utc_tm.tm_year + 1900,
            utc_tm.tm_mon + 1,
            utc_tm.tm_mday,
            utc_tm.tm_hour,
            utc_tm.tm_min,
            utc_tm.tm_sec);
        ESP_LOGI(APP_TAG, "RTC chip updated from NTP (UTC written)");
    }
}

static bool sync_time_from_rtc_or_ntp(void)
{
    const uint32_t NTP_SYNC_INTERVAL = 24 * 3600;
    time_t system_time_utc = time(NULL);              // System time is always UTC
    bool time_valid = (system_time_utc > 1577836800); // Check if year >= 2020

    if (rtc_dev != NULL)
    {
        RtcDateTime_t rtc_time_utc = rtc_dev->get_rtcTime(); // RTC stores UTC time
        if (rtc_time_utc.year >= 2024 && rtc_time_utc.year <= 2100)
        {
            struct tm utc_tm = {0};
            utc_tm.tm_year = rtc_time_utc.year - 1900;
            utc_tm.tm_mon = rtc_time_utc.month - 1;
            utc_tm.tm_mday = rtc_time_utc.day;
            utc_tm.tm_hour = rtc_time_utc.hour;
            utc_tm.tm_min = rtc_time_utc.minute;
            utc_tm.tm_sec = rtc_time_utc.second;
            utc_tm.tm_isdst = 0; // RTC stores UTC, no DST

            // Use centralized UTC-to-epoch conversion (SINGLE SOURCE OF TRUTH)
            time_t system_time_utc = Utilities::utc_tm_to_epoch(&utc_tm);

            if (system_time_utc > 0)
            {
                struct timeval tv = {.tv_sec = system_time_utc, .tv_usec = 0};
                settimeofday(&tv, NULL);

                ESP_LOGI(APP_TAG, "System time set from RTC (UTC): %04d-%02d-%02d %02d:%02d:%02d",
                         rtc_time_utc.year, rtc_time_utc.month, rtc_time_utc.day,
                         rtc_time_utc.hour, rtc_time_utc.minute, rtc_time_utc.second);
                time_valid = true;
            }
            else
            {
                ESP_LOGE(APP_TAG, "Failed to convert RTC time to epoch");
            }
        }
        else
        {
            ESP_LOGW(APP_TAG, "RTC time invalid (year=%d)", rtc_time_utc.year);
        }
    }

    uint32_t time_since_last_sync = time_valid ? ((uint32_t)time(NULL) - s_last_ntp_sync_time) : UINT32_MAX;
    bool need_ntp_sync = !time_valid || (time_since_last_sync > NTP_SYNC_INTERVAL);

    if (need_ntp_sync)
    {
        ESP_LOGI(APP_TAG, "NTP sync needed (time_valid=%d, last_sync=%u sec ago)",
                 time_valid, time_since_last_sync);
        return false;
    }

    ESP_LOGI(APP_TAG, "Time already synced (last NTP sync %u sec ago)", time_since_last_sync);
    return true;
}

static void start_sntp(void)
{
    ESP_LOGI(APP_TAG, "Starting SNTP for time synchronization");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_setservername(2, "time.google.com");

    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

static void initialize_sntp(void)
{
    setenv("TZ", Settings::instance().timezone(), 1);
    tzset();
    ESP_LOGI(APP_TAG, "Timezone: %s", Settings::instance().timezone());

    if (sync_time_from_rtc_or_ntp())
    {
        ESP_LOGI(APP_TAG, "Time already synchronized, SNTP not started");
        return;
    }

    start_sntp();
}

extern "C" void user_ntp_sync_if_due(uint32_t wait_ms)
{
    const uint32_t NTP_SYNC_INTERVAL = 24 * 3600;
    const uint32_t now = (uint32_t)time(NULL);
    const bool time_valid = (now > 1577836800); // after Jan 1, 2020
    if (time_valid && now - s_last_ntp_sync_time < NTP_SYNC_INTERVAL)
    {
        return;
    }

    if (!esp_sntp_enabled())
    {
        start_sntp();
    }
    for (uint32_t waited = 0; !time_synced && waited < wait_ms; waited += 100)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!time_synced)
    {
        ESP_LOGW(APP_TAG, "NTP sync not done in %u ms - will try on a later wake", (unsigned)wait_ms);
    }
}

static void led_test_task(void *arg)
{
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = 0x1ULL << 3;
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;

    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    for (;;)
    {
        gpio_set_level((gpio_num_t)3, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level((gpio_num_t)3, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// How long the WiFi setup portal waits for the user before the device goes on offline.
// Without a limit, a router outage at power-on keeps the radio on until the battery is empty.
static constexpr int CONFIG_PORTAL_TIMEOUT_S = 180;

/**
 * @brief Connect WiFi on a cold boot, with the setup portal as the fallback.
 *
 * Deep sleep wakes do not come here: fetch_data_cycle() connects only when it
 * needs the network, with a fast connect and a time limit.
 */
static void handle_wifi(void)
{
    wm.setConfigPortalStartedCallback([]()
                                      {
                                          ESP_LOGI(TAG, "Captive portal started - drawing connection instructions");
                                          if (wifi_ap_msg_handle == NULL)
                                          {
                                              xTaskCreatePinnedToCore(
                                                  [](void *)
                                                  {
                                                      // Draw once when portal starts
                                                      if (s_epd_mutex && xSemaphoreTake(s_epd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE)
                                                      {
                                                          String ssid = wm.getConfigPortalSSID();
                                                          render_captive_portal_screen(ssid.c_str());
                                                          xSemaphoreGive(s_epd_mutex);
                                                      }

                                                      // Stay alive while portal active; exit afterwards
                                                      while (wm.getConfigPortalActive() || wm.getWebPortalActive())
                                                      {
                                                          vTaskDelay(pdMS_TO_TICKS(1000));
                                                      }

                                                      wifi_ap_msg_handle = NULL;
                                                      vTaskDelete(NULL);
                                                  },
                                                  "wifi_ap_msg",
                                                  12288,   // larger stack for FT + SPI work
                                                  NULL,
                                                  5,
                                                  &wifi_ap_msg_handle,
                                                  1);      // pin to APP core

                                                  xTaskCreatePinnedToCore(led_test_task, "led_test_task", 4 * 1024, NULL, 4, NULL,0);
                                          } });

    // A slow AP should not push us into the portal too early.
    wm.setConnectTimeout(20);
    wm.setConnectRetries(3);
    wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S);

    ESP_LOGI(TAG, "Attempting autoConnect");
    bool connected = wm.autoConnect("ePaper-Config", NULL);
    if (!connected)
    {
        // The portal timed out or failed. Try the saved network once more without it.
        wifi_config_t conf{};
        if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != '\0')
        {
            ESP_LOGW(TAG, "autoConnect failed - trying plain reconnect to saved SSID");
            WiFi.mode(WIFI_STA);
            WiFi.begin();
            connected = (WiFi.waitForConnectResult(10000) == WL_CONNECTED);
            if (connected)
            {
                WiFi.softAPdisconnect(false);
            }
        }
        if (!connected)
        {
            ESP_LOGW(TAG, "WiFi not connected - continuing offline");
        }
    }

    if (connected)
    {
        ESP_LOGI(TAG, "WiFi connected: STA IP=%s", WiFi.localIP().toString().c_str());
        fetch_remember_wifi_ap();
    }

    // When autoConnect exits, ensure we cleanup the message task if still running
    if (wifi_ap_msg_handle != NULL)
    {
        vTaskDelete(wifi_ap_msg_handle);
        wifi_ap_msg_handle = NULL;
    }
}
/**
 * @brief Display the device IP for 3 seconds when web server starts.
 *
 * Called from button handler when web server is started successfully.
 * Creates a task that displays the IP, waits 3 seconds, then cleans up.
 */
static void display_web_server_ip(const char *audio_file)
{
    // Clean up any existing IP display task
    if (web_server_ip_display_handle != NULL)
    {
        vTaskDelete(web_server_ip_display_handle);
        web_server_ip_display_handle = NULL;
    }

    // Create new task to display IP for 3 seconds
    // Use a capture to pass audio_file to the lambda
    struct DisplayIPContext
    {
        char audio_file[64];
    };
    DisplayIPContext *ctx = new DisplayIPContext();
    if (audio_file)
    {
        strncpy(ctx->audio_file, audio_file, sizeof(ctx->audio_file) - 1);
        ctx->audio_file[sizeof(ctx->audio_file) - 1] = '\0';
    }
    else
    {
        ctx->audio_file[0] = '\0';
    }

    // Set flag BEFORE creating task to prevent race condition with main loop
    s_skip_next_display_update = true;
    ESP_LOGI(TAG, "IP display: set skip flag before task creation");

    xTaskCreate([](void *arg)
                {
                    DisplayIPContext *ctx = (DisplayIPContext *)arg;
                    
                    // Wait for WiFi connection (up to 10 seconds)
                    int wifi_wait_count = 0;
                    const int max_wait_iterations = 500; // 10 seconds (100 * 100ms)
                    
                    while (!WiFi.isConnected() && wifi_wait_count < max_wait_iterations)
                    {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        wifi_wait_count++;
                    }
                    
                    // Display the IP address
                    String ip = WiFi.localIP().toString();
                    ESP_LOGI(TAG, "Displaying web server IP: %s (waited %dms for WiFi)", 
                             ip.c_str(), wifi_wait_count * 100);

                    if (xSemaphoreTake(s_epd_mutex, pdMS_TO_TICKS(5000)) == pdTRUE)
                    {
                        // Initialize EPD if main loop skipped it
                        ESP_LOGI(TAG, "IP display: initializing EPD and displaying IP");
                        EPaper::get_instance().init();
                        
                        // Use simple text rendering to avoid FreeType issues
                        EPaper::get_instance().clear_buffer();
                        // Display the following message:
                        
                        // Connect to:
                        // http://
                        // <IP_ADDRESS>:9123

                        // Display the following message:
                        // Connect to:
                        // http://
                        // <IP_ADDRESS>:9123

                        EPaper::get_instance().draw_text5x7_center(30, "Connect to:", 2);
                        EPaper::get_instance().draw_text5x7_center(70, "http://", 2);
                        char ip_with_port[48];
                        snprintf(ip_with_port, sizeof(ip_with_port), "%s", ip.c_str());
                        EPaper::get_instance().draw_text5x7_center(110, ip_with_port, 2);
                        EPaper::get_instance().draw_text5x7_center(140, ":9123", 2);
                        
                        
                        // Use full refresh to ensure clean display
                        EPaper::get_instance().display_full_refresh();  
                        // Keep IP visible for 20 seconds
                        vTaskDelay(pdMS_TO_TICKS(20000)); // 20 seconds
                        // Clear flag after display time completes
                        s_skip_next_display_update = false;
                        xSemaphoreGive(s_epd_mutex);
                    }
                    else
                    {
                        // Failed to get mutex - clear flag anyway to not block forever
                        ESP_LOGW(TAG, "IP display: failed to get mutex, clearing skip flag");
                        s_skip_next_display_update = false;
                    }
                    
                    // Now that display is done, play audio if provided
                    if (ctx->audio_file[0] != '\0')
                    {
                        audio_player_play_nonblocking(ctx->audio_file);
                    }

                    // Clean up
                    web_server_ip_display_handle = NULL;
                    delete ctx;
                    vTaskDelete(NULL); },
                "web_srv_ip", 4096, ctx, 5, &web_server_ip_display_handle);
}

static void handle_deep_sleep_button_wake(void)
{
    if (s_reset_reason != ESP_RST_DEEPSLEEP || s_wakeup_cause != ESP_SLEEP_WAKEUP_EXT1)
    {
        return;
    }

    ESP_LOGI(APP_TAG, "Capturing button wake state for deferred handling");
    s_button_wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
    if (s_button_wakeup_pin_mask == 0)
    {
        ESP_LOGW(APP_TAG, "Button wake but no pin identified");
        s_button_wakeup_pin_mask = 0;
    }
}

static void process_deferred_button_wake(void)
{
    if (s_button_wakeup_pin_mask == 0)
    {
        return;
    }

    uint64_t wakeup_mask = s_button_wakeup_pin_mask;
    s_button_wakeup_pin_mask = 0;

    int boot_button_pressed = (wakeup_mask & (1ULL << BOOT_BUTTON_PIN)) != 0;

    if (boot_button_pressed)
    {
        ESP_LOGI(APP_TAG, "Processing deferred BOOT button wake -> enqueue BOOT_SINGLE");
        if (s_button_event_queue)
        {
            button_event_t evt = BTN_EVT_BOOT_SINGLE;
            (void)xQueueSend(s_button_event_queue, &evt, 0);
        }
    }
}

void init_led_pin(bool led_on = false)
{
    // Initialize LED pin (active-low)
    gpio_config_t led_config = {};
    led_config.pin_bit_mask = (1ULL << GREEN_LED_PIN);
    led_config.mode = GPIO_MODE_OUTPUT;
    led_config.pull_up_en = GPIO_PULLUP_ENABLE;
    led_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    led_config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&led_config);
    gpio_set_level((gpio_num_t)GREEN_LED_PIN, led_on ? 0 : 1); // LED off
}

void user_app_init(esp_reset_reason_t reset_reason)
{
    s_reset_reason = reset_reason;
    s_wakeup_cause = esp_sleep_get_wakeup_cause();
    const bool cold_boot = (reset_reason != ESP_RST_DEEPSLEEP);

    // Initialize LED pin early to ensure stable state (GPIO 3 is also JTAG strap)
    init_led_pin(true);

    ESP_LOGI(TAG, "Loading settings...");
    Settings::instance().load();

    // Apply timezone as early as possible so any localtime() calls reflect user settings
    setenv("TZ", Settings::instance().timezone(), 1);
    tzset();
    ESP_LOGI(APP_TAG, "Timezone (early): %s", Settings::instance().timezone());

    // Set system time from RTC only; defer SNTP until after WiFi decision.
    // This ensures our offline market-open check uses RTC time without
    // attempting any network activity.
    (void)sync_time_from_rtc_or_ntp();

    // Load cached data (including last known market state and next close time)
    // from NVS so we can decide whether to bring up WiFi without any network.
    // Note: user_ui_init() also calls load_from_nvs(); reloading is harmless
    // and ensures we have the persisted values before WiFi decisions.
    DataCache::instance().load_from_nvs();

    // Apply boot wakeup policy to restore stock carousel index from RTC memory
    // This must happen after NVS load and before any data fetching
    apply_boot_wakeup_policy();

    // Initialize buttons
    user_button_init();

    // Mount SPIFFS
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true};
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
        ESP_LOGE("SPIFFS", "Mount failed (%s)", esp_err_to_name(ret));
    else
        ESP_LOGI("SPIFFS", "SPIFFS mounted");

    // Initialize log capture system (after SPIFFS is mounted)
    if (log_capture_init() == ESP_OK)
    {
        ESP_LOGI(TAG, "Log capture system initialized");
    }
    else
    {
        ESP_LOGW(TAG, "Failed to initialize log capture - logs will go to serial only");
    }

    // Audio starts on first use (AudioPlayer::play). Only the power-on sound needs it here;
    // a timer wake does not pay for the codec start-up.
    if (Settings::instance().sound_enabled() && reset_reason == ESP_RST_POWERON)
    {
        esp_err_t er = AudioPlayer::instance().play("startup.mp3", false);
        if (er != ESP_OK)
        {
            ESP_LOGW(TAG, "Startup sound failed: %s", esp_err_to_name(er));
        }
    }

    // Web server does NOT auto-start (user must enable via PWR button double-click)
    ESP_LOGI(TAG, "Web server disabled by default. Double-click PWR button to start.");

    // Create EPD mutex
    if (!s_epd_mutex)
        s_epd_mutex = xSemaphoreCreateMutex();

    // Create unified button event queue and background handler task
    if (s_button_event_queue == nullptr)
    {
        s_button_event_queue = xQueueCreate(8, sizeof(button_event_t));
        if (s_button_event_queue)
        {
            button_register_event_queue(s_button_event_queue);
            xTaskCreatePinnedToCore(button_event_task, "btn_events", 4096, NULL, 6, NULL, 1);
            ESP_LOGI(TAG, "Button event task started");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to create button event queue");
        }
    }

    // Dynamic frequency scaling. Light sleep stays off: it would release the
    // e-paper and audio power pads (CONFIG_PM_SLP_DISABLE_GPIO).
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = 240;
    pm_config.min_freq_mhz = 80;
    pm_config.light_sleep_enable = false;
    ret = esp_pm_configure(&pm_config);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Power management configured (DFS)");
    }
    else
    {
        ESP_LOGI(TAG, "PM configuration failed: %s", esp_err_to_name(ret));
    }

    // On a cold boot connect now (with the setup portal if needed) and sync the clock.
    // A deep sleep wake connects later in fetch_data_cycle(), only if it needs the network.
    if (cold_boot)
    {
        handle_wifi();
        if (WiFi.isConnected())
        {
            initialize_sntp();
        }
    }

    handle_deep_sleep_button_wake();
    // Route any deferred button wake into the same handler
    process_deferred_button_wake();

    ESP_LOGI(TAG, "user_app_init complete");
}

static bool apply_boot_wakeup_policy(void)
{
    s_cached_data_ready_on_boot = false;

    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    bool timer_wake = (s_reset_reason == ESP_RST_DEEPSLEEP) && (wake_cause == ESP_SLEEP_WAKEUP_TIMER);

    if (timer_wake && s_rtc_cache_preserved)
    {
        uint8_t stock_count = Settings::instance().stock_count();
        if (stock_count > 0)
        {
            uint8_t idx = s_rtc_next_stock_index % stock_count;
            DataCache::instance().set_current_stock_index(idx);
            ESP_LOGI(APP_TAG, "Timer wake - restored stock index %u", idx);

            bool market_open = DataCache::instance().get_market_open();
            if (market_open)
            {
                ESP_LOGI(APP_TAG, "Market open - will fetch fresh data");
                s_rtc_cache_preserved = false;
                s_rtc_expected_sleep_ms = 0;
                return true;
            }
            else
            {
                stock_data_t cached = DataCache::instance().get_stock_data(idx);
                if (cached.valid)
                {
                    ESP_LOGI(APP_TAG, "Market closed - using cached data for stock %u", idx);
                    s_cached_data_ready_on_boot = true;
                    s_rtc_cache_preserved = false;
                    s_rtc_expected_sleep_ms = 0;
                    return false;
                }
                else
                {
                    ESP_LOGW(APP_TAG, "Market closed but no cached data - fetching anyway");
                    s_rtc_cache_preserved = false;
                    s_rtc_expected_sleep_ms = 0;
                    return true;
                }
            }
        }
        else
        {
            ESP_LOGI(APP_TAG, "Timer wake with no configured stocks");
        }
    }
    else
    {
        if (timer_wake && !s_rtc_cache_preserved)
        {
            ESP_LOGW(APP_TAG, "Timer wake without preserved cache - invalidating");
        }
        else
        {
            ESP_LOGI(APP_TAG, "Non-timer wake (cause=%d, reset=%d) - cache will be refreshed",
                     (int)wake_cause, (int)s_reset_reason);
        }
        // Cache will be refreshed naturally by fetch task
        DataCache::instance().set_current_stock_index(0);
    }

    s_rtc_cache_preserved = false;
    s_rtc_expected_sleep_ms = 0;
    return true;
}

void user_ui_init(void)
{
    // Initialize fetch module with RTC device
    fetch_init(rtc_dev);

    // Start main loop task (core 1, priority 5)
    // Note: fetch is now called synchronously from within the loop, not as a separate task
    xTaskCreatePinnedToCore(loop_direct_ft, "main_loop", 24 * 1024, nullptr, 5, &loop_direct_ft_task_handle, 1);
}

// ============================================================================
// C INTERFACE FUNCTIONS
// ============================================================================

extern "C" void user_request_refresh(void)
{
    fetch_request_weather_refresh();
    user_abort_main_delay();
}

extern "C" void user_set_device_sleep_enabled(bool enabled)
{
    // Update settings data with new sleep state
    SettingsData data = Settings::instance().get();
    data.device_sleep_enabled = enabled;
    Settings::instance().update(data);
    Settings::instance().save();
}

extern "C" void user_set_dead_battery_image_test_mode(bool enabled)
{
    s_dead_battery_image_test_mode = enabled;
    user_abort_main_delay();
}

extern "C" bool user_get_dead_battery_image_test_mode(void)
{
    return s_dead_battery_image_test_mode;
}

extern "C" void user_abort_main_delay(void)
{
    if (loop_direct_ft_task_handle != NULL)
    {
        xTaskAbortDelay(loop_direct_ft_task_handle);
        (void)xTaskNotify(loop_direct_ft_task_handle, 0x1, eSetBits);
    }
}
