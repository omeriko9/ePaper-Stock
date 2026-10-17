#include "fetch_task.h"
#include "data_cache.h"
#include "utilities.h"
#include "settings.h"
#include "audio_player.h"
#include "user_app.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "web_server.h"
#include <WiFi.h>
#include <time.h>
#include <string.h>
#include <algorithm>
#include "esp_attr.h"

static const char *TAG = "fetch";
static i2c_equipment *s_rtc_dev = nullptr;
static bool s_was_market_open = false;  // Track previous market state
static bool s_wifi_connection_requested = false; // Flag to force WiFi connection on next cycle
static bool s_weather_refresh_requested = false;

// Weather and the slower extended-hours stock updates run on this interval.
static constexpr uint32_t WEATHER_INTERVAL_S = 15 * 60;

// Upper limit for one WiFi connection attempt. A wake must not hang on a router
// that is down; the next wake tries again.
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
static constexpr uint32_t WIFI_FAST_CONNECT_TIMEOUT_MS = 4000;

static constexpr uint32_t NTP_WAIT_MS = 3000;

// RTC retained variables to track prices across deep sleep cycles
// This is more reliable than NVS for frequent updates
RTC_DATA_ATTR static double s_rtc_last_prices[10] = {0};
RTC_DATA_ATTR static bool s_rtc_last_prices_valid[10] = {0};

// After a failed weather fetch, wait this long before the next try, so that a
// server problem does not turn the radio on at every wake.
static constexpr uint32_t WEATHER_RETRY_S = 15 * 60;
RTC_DATA_ATTR static uint32_t s_rtc_weather_retry_after = 0;

// Last access point, so a wake can skip the channel scan.
RTC_DATA_ATTR static uint8_t s_rtc_ap_bssid[6] = {0};
RTC_DATA_ATTR static uint8_t s_rtc_ap_channel = 0;
RTC_DATA_ATTR static bool s_rtc_ap_valid = false;

void fetch_init(i2c_equipment *rtc_dev)
{
    s_rtc_dev = rtc_dev;
    ESP_LOGI(TAG, "Fetch module initialized");
}

void fetch_request_wifi_connection(void)
{
    s_wifi_connection_requested = true;
    ESP_LOGI(TAG, "WiFi connection requested for next fetch cycle");
}

void fetch_request_weather_refresh(void)
{
    s_weather_refresh_requested = true;
    ESP_LOGI(TAG, "Weather refresh requested for next fetch cycle");
}

void fetch_remember_wifi_ap(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        memcpy(s_rtc_ap_bssid, ap.bssid, sizeof(s_rtc_ap_bssid));
        s_rtc_ap_channel = ap.primary;
        s_rtc_ap_valid = true;
    }
}

static uint32_t elapsed_ms_since(uint32_t start_ms)
{
    return (uint32_t)millis() - start_ms;
}

/**
 * @brief Connect to the saved network, with a time limit.
 *
 * First try the access point from the last wake (no scan, usually well under a
 * second), then fall back to a normal connect with a scan.
 */
static bool wifi_connect(uint32_t timeout_ms)
{
    if (WiFi.isConnected())
    {
        return true;
    }

    // Do not write the WiFi config to NVS on every wake.
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);

    wifi_config_t conf{};
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || conf.sta.ssid[0] == '\0')
    {
        ESP_LOGW(TAG, "No saved WiFi network");
        return false;
    }
    // The config fields are not always NUL-terminated.
    char ssid[sizeof(conf.sta.ssid) + 1] = {0};
    char pass[sizeof(conf.sta.password) + 1] = {0};
    memcpy(ssid, conf.sta.ssid, sizeof(conf.sta.ssid));
    memcpy(pass, conf.sta.password, sizeof(conf.sta.password));

    const uint32_t start_ms = millis();

    if (s_rtc_ap_valid)
    {
        ESP_LOGI(TAG, "WiFi: fast connect (channel %u)", s_rtc_ap_channel);
        WiFi.begin(ssid, pass, s_rtc_ap_channel, s_rtc_ap_bssid);
        if (WiFi.waitForConnectResult(std::min(timeout_ms, WIFI_FAST_CONNECT_TIMEOUT_MS)) == WL_CONNECTED)
        {
            ESP_LOGI(TAG, "WiFi connected in %u ms", (unsigned)elapsed_ms_since(start_ms));
            return true;
        }
        ESP_LOGW(TAG, "WiFi: fast connect failed - scanning");
        s_rtc_ap_valid = false;
        WiFi.disconnect();
    }

    const uint32_t used_ms = elapsed_ms_since(start_ms);
    if (used_ms >= timeout_ms)
    {
        return false;
    }
    WiFi.begin(ssid, pass);
    if (WiFi.waitForConnectResult(timeout_ms - used_ms) == WL_CONNECTED)
    {
        fetch_remember_wifi_ap();
        ESP_LOGI(TAG, "WiFi connected in %u ms", (unsigned)elapsed_ms_since(start_ms));
        return true;
    }
    return false;
}

static void wifi_off(void)
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void fetch_data_cycle(void)
{
    ESP_LOGI(TAG, "=== Starting data fetch cycle ===");

    // Always update time from RTC on wake — no internet required
    time_data_t time_data{};
    if (Utilities::fetch_time_data(s_rtc_dev, &time_data))
    {
        DataCache::instance().set_time_data(time_data);
        ESP_LOGI(TAG, "RTC Time: %02d:%02d:%02d", time_data.hour, time_data.minute, time_data.second);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to update time from RTC");
    }

    // The battery level is on screen on every wake, so read it on every wake.
    uint8_t battery_level = Utilities::fetch_battery_level();
    DataCache::instance().set_battery_level(battery_level);
    ESP_LOGI(TAG, "Battery: %d%%", battery_level);

    time_t now = time(NULL);
    bool time_valid = (time_data.year >= 2020);
    bool mock_mode = Settings::instance().mock_mode_enabled();

    // Weather age. If time is invalid, treat it as old so that we connect and
    // get fresh data and time.
    bool weather_stale = true;
    if (time_valid)
    {
        uint32_t last_weather_ts = DataCache::instance().get_weather_last_fetch_ts();
        weather_stale = ((uint32_t)now >= last_weather_ts + WEATHER_INTERVAL_S);
    }
    else
    {
        ESP_LOGW(TAG, "System time invalid (<2020) - forcing network fetch");
    }

    // Determine market status from local time (no network needed).
    bool market_active = false;
    if (mock_mode)
    {
        market_active = true;  // Mock mode treats market as always active
        ESP_LOGI(TAG, "Mock mode enabled - treating market as active");
    }
    else
    {
        market_active = Utilities::is_us_market_open_local();

        bool cached_status = DataCache::instance().get_market_open();
        if (weather_stale || market_active != cached_status)
        {
            ESP_LOGI(TAG, "Market status: %s %s",
                     market_active ? "ACTIVE" : "CLOSED",
                     market_active != cached_status ? "(CHANGED)" : "");
            DataCache::instance().set_market_open(market_active);
            DataCache::instance().set_market_last_check_ts((uint32_t)now);
        }
    }

    // The screen shows weather only while the market is closed. During market
    // hours a weather fetch costs two HTTPS requests for nothing.
    bool weather_retry_ok = (uint32_t)now >= s_rtc_weather_retry_after;
    bool weather_due = (weather_stale && !market_active && weather_retry_ok) || s_weather_refresh_requested;

    // Extended hours: pre-market 4:00-9:30 ET, post-market 16:00-20:00 ET
    bool in_extended_hours = !mock_mode && Utilities::is_extended_hours_active();

    // Stock fetch policy:
    // - Regular market hours: every wake (prices change rapidly)
    // - Extended hours: on the weather interval (every 15 min)
    // - Market closed (outside extended hours): only if the cache is invalid
    uint8_t current_idx = DataCache::instance().get_current_stock_index();
    stock_data_t current_stock = DataCache::instance().get_stock_data(current_idx);

    bool stock_needs_update = false;
    if (market_active)
    {
        stock_needs_update = true;
        ESP_LOGI(TAG, "Market active - will fetch stock data");
    }
    else if (in_extended_hours && weather_stale)
    {
        stock_needs_update = true;
        ESP_LOGI(TAG, "In extended hours + weather interval - will fetch stock for pre/post-market prices");
    }
    else if (!current_stock.valid)
    {
        stock_needs_update = true;
        ESP_LOGI(TAG, "Stock cache invalid - will fetch");
    }

    bool need_internet = stock_needs_update || weather_due || s_wifi_connection_requested;
    if (!need_internet)
    {
        ESP_LOGI(TAG, "No internet needed (%s)",
                 in_extended_hours ? "extended hours, waiting for weather interval"
                                   : "market closed, stock valid, weather fresh");
        return;
    }

    ESP_LOGI(TAG, "Internet needed - connecting WiFi...");
    if (!wifi_connect(WIFI_CONNECT_TIMEOUT_MS))
    {
        ESP_LOGW(TAG, "WiFi connect failed - skipping network operations");
        s_wifi_connection_requested = false;
        if (!web_server_is_running())
        {
            wifi_off();
        }
        return;
    }

    if (s_wifi_connection_requested)
    {
        // Give the DNS resolver time to be ready for the web server use case.
        ESP_LOGI(TAG, "WiFi connection request fulfilled - waiting 2s for DNS");
        vTaskDelay(pdMS_TO_TICKS(2000));
        s_wifi_connection_requested = false;
    }

    // Track market transitions for audio alerts
    bool is_deep_sleep_wakeup = (esp_reset_reason() == ESP_RST_DEEPSLEEP);
    if (market_active && !s_was_market_open && Settings::instance().sound_enabled() && !is_deep_sleep_wakeup)
    {
        ESP_LOGI(TAG, "Market transitioned to active - playing sound");
        AudioPlayer::instance().play("market_open.mp3", false);
    }
    s_was_market_open = market_active;

    if (stock_needs_update)
    {
        uint8_t stock_count = Settings::instance().stock_count();
        const StockEntry *stocks = Settings::instance().stocks();

        if (current_idx < stock_count)
        {
            const char *symbol = stocks[current_idx].symbol;
            if (symbol && symbol[0] != '\0')
            {
                const char *reason = market_active ? "market active"
                                   : (in_extended_hours ? "extended hours + weather interval" : "invalid cache");
                ESP_LOGI(TAG, "Fetching stock[%u] data for %s (%s)...", current_idx, symbol, reason);

                stock_data_t stock{};
                if (Utilities::fetch_stock_data(symbol, &stock))
                {
                    // Get previous stock data from cache BEFORE updating it
                    stock_data_t prev_stock = DataCache::instance().get_stock_data(current_idx);

                    DataCache::instance().set_stock_data(stock, current_idx);
                    DataCache::instance().set_stock_last_fetch_ts((uint32_t)now, current_idx);

                    if (stock.has_premarket)
                    {
                        ESP_LOGI(TAG, "Stock[%u] %s: %s (%s) | Extended: %s (%s)",
                                 current_idx, symbol,
                                 stock.current_price_str, stock.change_percent_str,
                                 stock.premarket_price_str, stock.premarket_change_percent_str);
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Stock[%u] %s: %s (%s) | No extended hours data",
                                 current_idx, symbol, stock.current_price_str, stock.change_percent_str);
                    }

                    // Price threshold alerts. The previous price comes from RTC memory
                    // (survives deep sleep), else from the cache, to avoid repeated alerts.
                    double prev_price_val = 0.0;
                    bool prev_price_valid = false;
                    if (s_rtc_last_prices_valid[current_idx])
                    {
                        prev_price_val = s_rtc_last_prices[current_idx];
                        prev_price_valid = true;
                    }
                    else if (prev_stock.valid)
                    {
                        prev_price_val = prev_stock.current_price;
                        prev_price_valid = true;
                    }

                    s_rtc_last_prices[current_idx] = stock.current_price;
                    s_rtc_last_prices_valid[current_idx] = true;

                    if (Settings::instance().sound_enabled() && stock.valid && prev_price_valid)
                    {
                        float max_threshold = stocks[current_idx].max_sound;
                        float min_threshold = stocks[current_idx].min_sound;
                        double current_price = stock.current_price;

                        // Alert if we CROSSED the threshold from below
                        if (max_threshold > 0.0f && current_price >= max_threshold && prev_price_val < max_threshold)
                        {
                            ESP_LOGI(TAG, "Stock price %.2f exceeded threshold %.2f (prev: %.2f) - playing high sound",
                                     current_price, max_threshold, prev_price_val);
                            AudioPlayer::instance().play("stock_high.mp3", false);
                        }

                        // Alert if we CROSSED the threshold from above
                        if (min_threshold > 0.0f && current_price <= min_threshold && prev_price_val > min_threshold)
                        {
                            ESP_LOGI(TAG, "Stock price %.2f fell below threshold %.2f (prev: %.2f) - playing low sound",
                                     current_price, min_threshold, prev_price_val);
                            AudioPlayer::instance().play("stock_low.mp3", false);
                        }
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "Failed to fetch stock data for %s", symbol);
                    stock_data_t invalid{};
                    invalid.valid = false;
                    DataCache::instance().set_stock_data(invalid, current_idx);
                }
            }
        }
    }
    else
    {
        ESP_LOGI(TAG, "Stock fetch skipped - market closed and cache valid");
    }

    if (weather_due)
    {
        ESP_LOGI(TAG, "Fetching current weather...");
        weather_data_t weather{};
        if (Utilities::fetch_current_weather_data(&weather))
        {
            DataCache::instance().set_weather_current_data(weather);
            DataCache::instance().set_weather_last_fetch_ts((uint32_t)now);
            s_rtc_weather_retry_after = 0;
            ESP_LOGI(TAG, "Weather: %s, %s", weather.condition, weather.temperature);
        }
        else
        {
            s_rtc_weather_retry_after = (uint32_t)now + WEATHER_RETRY_S;
            ESP_LOGW(TAG, "Failed to fetch current weather - next try in %u min", (unsigned)(WEATHER_RETRY_S / 60));
        }
        s_weather_refresh_requested = false;
    }

    // Market open/close times feed the countdown on screen. A known future time stays
    // correct until it passes, so ask the server only then.
    if (DataCache::instance().get_next_market_open_time() <= now)
    {
        if (Utilities::fetch_next_market_open_time() <= 0)
        {
            ESP_LOGW(TAG, "Failed to fetch next market open time");
        }
    }
    if (DataCache::instance().get_next_market_close_time() <= now)
    {
        time_t next_close = Utilities::fetch_next_market_close_time();
        if (next_close > 0)
        {
            DataCache::instance().set_next_market_close_time(next_close);
        }
        else
        {
            ESP_LOGW(TAG, "Failed to fetch next market close time");
        }
    }

    user_ntp_sync_if_due(NTP_WAIT_MS);

    DataCache::instance().save_to_nvs();

    // Turn the radio off as soon as we are done. Keep it on only for the web server.
    if (!web_server_is_running())
    {
        ESP_LOGI(TAG, "WiFi off");
        wifi_off();
    }
    else
    {
        ESP_LOGI(TAG, "Web server active - keeping WiFi connected");
    }

    ESP_LOGI(TAG, "=== Fetch cycle complete ===");
}
