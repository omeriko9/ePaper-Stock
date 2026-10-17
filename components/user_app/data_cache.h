#pragma once

#include <mutex>
#include <nvs_flash.h>
#include <nvs.h>
#include "utilities.h"

/**
 * @brief Thread-safe data cache with NVS persistence for sharing fetched data
 * between fetch task and UI task.
 * 
 * This singleton manages cached stock, weather, time, and battery data.
 * Data is stored both in RAM (for fast access) and NVS (for persistence across reboots).
 * All operations are thread-safe via mutex protection.
 */
class DataCache {
public:
    static DataCache& instance();

    // Stock data (multi-stock support)
    void set_stock_data(const stock_data_t& data, uint8_t index = 0);
    stock_data_t get_stock_data(uint8_t index = 0);
    // Current stock carousel index
    void set_current_stock_index(uint8_t index);
    uint8_t get_current_stock_index();
    void advance_stock_index();  // Move to next stock in carousel
    void set_stock_last_fetch_ts(uint32_t ts, uint8_t index = 0);
    uint32_t get_stock_last_fetch_ts(uint8_t index = 0);
    
    // Weather data
    void set_weather_data(const weather_data_t& data);
    weather_data_t get_weather_data();
    // 15‑minute current weather
    void set_weather_current_data(const weather_data_t& data);
    weather_data_t get_weather_current_data();
    void set_weather_last_fetch_ts(uint32_t ts);
    uint32_t get_weather_last_fetch_ts();
    void invalidate_weather_cache();
    
    // Time data
    void set_time_data(const time_data_t& data);
    time_data_t get_time_data();
    
    // Battery level
    void set_battery_level(uint8_t level);
    uint8_t get_battery_level();
    
    // Market status
    void set_market_open(bool is_open);
    bool get_market_open();
    void set_market_last_check_ts(uint32_t ts);
    uint32_t get_market_last_check_ts();
    
    // Market close time
    void set_next_market_close_time(time_t close_time);
    time_t get_next_market_close_time();
    
    // Market open time
    void set_next_market_open_time(time_t open_time);
    time_t get_next_market_open_time();
    void set_market_open_last_fetch_ts(uint32_t ts);
    uint32_t get_market_open_last_fetch_ts();
    
    // Cache clearing (wipes all NVS cache data)
    bool clear_nvs_cache();
    
    // Load all cached data from NVS (called at startup)
    bool load_from_nvs();
    
    // Save all cached data to NVS (called after fetch)
    bool save_to_nvs();
    
    // Initialize - clear all data to defaults
    void init();

private:
    DataCache();
    ~DataCache() = default;
    
    // Prevent copying
    DataCache(const DataCache&) = delete;
    DataCache& operator=(const DataCache&) = delete;
    
    std::mutex mtx_;
    
    // Cached data (support up to 10 stocks)
    stock_data_t stock_data_[10];
    uint8_t current_stock_index_ = 0;
    weather_data_t weather_data_;
    weather_data_t weather_current_data_;
    time_data_t time_data_;
    uint8_t battery_level_;
    bool market_open_;
    time_t next_market_close_time_;
    time_t next_market_open_time_;
    // Per-stock timestamps (epoch seconds) - one entry per stock
    uint32_t ts_stock_last_fetch_[10] = {};
    uint32_t ts_weather_last_fetch_ = 0;
    uint32_t ts_market_last_check_ = 0;
    uint32_t ts_market_open_last_fetch_ = 0;
    
    // NVS helpers
    static const char* NVS_NAMESPACE;
    static const char* NVS_KEY_STOCK_VALID;
    static const char* NVS_KEY_STOCK_SYMBOL;
    static const char* NVS_KEY_STOCK_CURRENT;
    static const char* NVS_KEY_STOCK_PREV;
    static const char* NVS_KEY_STOCK_CHANGE;
    static const char* NVS_KEY_STOCK_HAS_PRE;
    static const char* NVS_KEY_STOCK_PRE_PRICE;
    static const char* NVS_KEY_STOCK_PRE_CHANGE;
    static const char* NVS_KEY_WEATHER_VALID;
    static const char* NVS_KEY_WEATHER_TEMP;
    static const char* NVS_KEY_WEATHER_COND;
    static const char* NVS_KEY_WEATHER_HUM;
    static const char* NVS_KEY_WEATHER_RAIN;
    // 15‑minute weather keys
    static const char* NVS_KEY_WCUR_VALID;
    static const char* NVS_KEY_WCUR_TEMP;
    static const char* NVS_KEY_WCUR_COND;
    static const char* NVS_KEY_WCUR_HUM;
    static const char* NVS_KEY_WCUR_RAIN;
    // Timestamp keys
    static const char* NVS_KEY_TS_STOCK;
    static const char* NVS_KEY_TS_WEATHER;
    static const char* NVS_KEY_TS_MARKET;
    static const char* NVS_KEY_BATTERY;
    static const char* NVS_KEY_MARKET_OPEN;
    static const char* NVS_KEY_MARKET_CLOSE_TIME;
    static const char* NVS_KEY_MARKET_OPEN_TIME;
    static const char* NVS_KEY_TS_MARKET_OPEN_FETCH;
};
