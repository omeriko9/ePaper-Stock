#include "data_cache.h"
#include "settings.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char* TAG = "data_cache";

// NVS namespace and keys
const char* DataCache::NVS_NAMESPACE = "data_cache";
const char* DataCache::NVS_KEY_STOCK_VALID = "stk_valid";
const char* DataCache::NVS_KEY_STOCK_SYMBOL = "stk_sym";
const char* DataCache::NVS_KEY_STOCK_CURRENT = "stk_cur";
const char* DataCache::NVS_KEY_STOCK_PREV = "stk_prev";
const char* DataCache::NVS_KEY_STOCK_CHANGE = "stk_chg";
const char* DataCache::NVS_KEY_STOCK_HAS_PRE = "stk_hpre";
const char* DataCache::NVS_KEY_STOCK_PRE_PRICE = "stk_pprice";
const char* DataCache::NVS_KEY_STOCK_PRE_CHANGE = "stk_pchg";
const char* DataCache::NVS_KEY_WEATHER_VALID = "wth_valid";
const char* DataCache::NVS_KEY_WEATHER_TEMP = "wth_temp";
const char* DataCache::NVS_KEY_WEATHER_COND = "wth_cond";
const char* DataCache::NVS_KEY_WEATHER_HUM = "wth_hum";
const char* DataCache::NVS_KEY_WEATHER_RAIN = "wth_rain";
// 15‑minute weather keys
const char* DataCache::NVS_KEY_WCUR_VALID = "wcur_valid";
const char* DataCache::NVS_KEY_WCUR_TEMP  = "wcur_temp";
const char* DataCache::NVS_KEY_WCUR_COND  = "wcur_cond";
const char* DataCache::NVS_KEY_WCUR_HUM   = "wcur_hum";
const char* DataCache::NVS_KEY_WCUR_RAIN  = "wcur_rain";
// Timestamp keys
const char* DataCache::NVS_KEY_TS_STOCK      = "ts_stock";
const char* DataCache::NVS_KEY_TS_WEATHER    = "ts_weather";
const char* DataCache::NVS_KEY_TS_MARKET     = "ts_mkt_chk";
const char* DataCache::NVS_KEY_BATTERY = "battery";
const char* DataCache::NVS_KEY_MARKET_OPEN = "mkt_open";
const char* DataCache::NVS_KEY_MARKET_CLOSE_TIME = "mkt_close_time";
const char* DataCache::NVS_KEY_MARKET_OPEN_TIME = "mkt_open_time";
const char* DataCache::NVS_KEY_TS_MARKET_OPEN_FETCH = "ts_mkt_open_fetch";

DataCache& DataCache::instance() {
    static DataCache cache;
    return cache;
}

DataCache::DataCache() {
    init();
}

void DataCache::init() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // Initialize stock data array
    for (int i = 0; i < 10; i++) {
        memset(&stock_data_[i], 0, sizeof(stock_data_[i]));
        stock_data_[i].valid = false;
    }
    current_stock_index_ = 0;
    
    // Initialize weather data
    memset(&weather_data_, 0, sizeof(weather_data_));
    weather_data_.valid = false;
    // 15‑minute current weather data
    memset(&weather_current_data_, 0, sizeof(weather_current_data_));
    weather_current_data_.valid = false;
    
    // Initialize time data
    memset(&time_data_, 0, sizeof(time_data_));
    
    // Initialize battery and market status
    battery_level_ = 0;
    market_open_ = false;
    next_market_close_time_ = 0;
    next_market_open_time_ = 0;
}

void DataCache::set_stock_data(const stock_data_t& data, uint8_t index) {
    if (index >= 10) return;
    std::lock_guard<std::mutex> lock(mtx_);
    stock_data_[index] = data;
    ESP_LOGI(TAG, "SET stock[%u]: valid=%d, price=%s, change=%s", 
             index, data.valid, data.current_price_str, data.change_percent_str);
}

stock_data_t DataCache::get_stock_data(uint8_t index) {
    if (index >= 10) {
        stock_data_t empty{};
        empty.valid = false;
        return empty;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    ESP_LOGI(TAG, "GET stock[%u]: valid=%d, price=%s", 
             index, stock_data_[index].valid, stock_data_[index].current_price_str);
    return stock_data_[index];
}

void DataCache::set_current_stock_index(uint8_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index < 10) {
        current_stock_index_ = index;
    }
}

uint8_t DataCache::get_current_stock_index() {
    std::lock_guard<std::mutex> lock(mtx_);
    return current_stock_index_;
}

void DataCache::advance_stock_index() {
    std::lock_guard<std::mutex> lock(mtx_);
    uint8_t stock_count = Settings::instance().stock_count();
    if (stock_count > 1) {
        current_stock_index_ = (current_stock_index_ + 1) % stock_count;
        ESP_LOGI(TAG, "Advanced stock carousel to index %u", current_stock_index_);
    }
}

void DataCache::set_weather_data(const weather_data_t& data) {
    std::lock_guard<std::mutex> lock(mtx_);
    weather_data_ = data;
    ESP_LOGI(TAG, "SET weather: valid=%d, temp=%s, cond=%s", 
             data.valid, data.temperature, data.condition);
}

weather_data_t DataCache::get_weather_data() {
    std::lock_guard<std::mutex> lock(mtx_);
    ESP_LOGI(TAG, "GET weather: valid=%d, cond=%s", 
             weather_data_.valid, weather_data_.condition);
    return weather_data_;
}

void DataCache::set_weather_current_data(const weather_data_t& data) {
    std::lock_guard<std::mutex> lock(mtx_);
    weather_current_data_ = data;
}

weather_data_t DataCache::get_weather_current_data() {
    std::lock_guard<std::mutex> lock(mtx_);
    return weather_current_data_;
}

void DataCache::set_stock_last_fetch_ts(uint32_t ts, uint8_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index < 10) {
        ts_stock_last_fetch_[index] = ts;
    }
}

uint32_t DataCache::get_stock_last_fetch_ts(uint8_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index < 10) {
        return ts_stock_last_fetch_[index];
    }
    return 0;
}



void DataCache::set_weather_last_fetch_ts(uint32_t ts) {
    std::lock_guard<std::mutex> lock(mtx_);
    ts_weather_last_fetch_ = ts;
}

uint32_t DataCache::get_weather_last_fetch_ts() {
    std::lock_guard<std::mutex> lock(mtx_);
    return ts_weather_last_fetch_;
}

void DataCache::invalidate_weather_cache() {
    std::lock_guard<std::mutex> lock(mtx_);
    memset(&weather_data_, 0, sizeof(weather_data_));
    weather_data_.valid = false;
    memset(&weather_current_data_, 0, sizeof(weather_current_data_));
    weather_current_data_.valid = false;
    ts_weather_last_fetch_ = 0;
    ESP_LOGI(TAG, "Weather cache invalidated");
}

void DataCache::set_time_data(const time_data_t& data) {
    std::lock_guard<std::mutex> lock(mtx_);
    time_data_ = data;
    ESP_LOGI(TAG, "SET time: %02d:%02d", data.hour, data.minute);
}

time_data_t DataCache::get_time_data() {
    std::lock_guard<std::mutex> lock(mtx_);
    ESP_LOGI(TAG, "GET time: %02d:%02d", time_data_.hour, time_data_.minute);
    return time_data_;
}

void DataCache::set_battery_level(uint8_t level) {
    std::lock_guard<std::mutex> lock(mtx_);
    battery_level_ = level;
    ESP_LOGI(TAG, "SET battery: %d%%", level);
}

uint8_t DataCache::get_battery_level() {
    std::lock_guard<std::mutex> lock(mtx_);
    ESP_LOGI(TAG, "GET battery: %d%%", battery_level_);
    return battery_level_;
}

void DataCache::set_market_open(bool is_open) {
    std::lock_guard<std::mutex> lock(mtx_);
    market_open_ = is_open;
}

bool DataCache::get_market_open() {
    std::lock_guard<std::mutex> lock(mtx_);
    return market_open_;
}

void DataCache::set_next_market_close_time(time_t close_time) {
    std::lock_guard<std::mutex> lock(mtx_);
    next_market_close_time_ = close_time;
}

time_t DataCache::get_next_market_close_time() {
    std::lock_guard<std::mutex> lock(mtx_);
    return next_market_close_time_;
}

void DataCache::set_next_market_open_time(time_t open_time) {
    std::lock_guard<std::mutex> lock(mtx_);
    next_market_open_time_ = open_time;
}

time_t DataCache::get_next_market_open_time() {
    std::lock_guard<std::mutex> lock(mtx_);
    return next_market_open_time_;
}

void DataCache::set_market_open_last_fetch_ts(uint32_t ts) {
    std::lock_guard<std::mutex> lock(mtx_);
    ts_market_open_last_fetch_ = ts;
}

uint32_t DataCache::get_market_open_last_fetch_ts() {
    std::lock_guard<std::mutex> lock(mtx_);
    return ts_market_open_last_fetch_;
}

void DataCache::set_market_last_check_ts(uint32_t ts) {
    std::lock_guard<std::mutex> lock(mtx_);
    ts_market_last_check_ = ts;
}

uint32_t DataCache::get_market_last_check_ts() {
    std::lock_guard<std::mutex> lock(mtx_);
    return ts_market_last_check_;
}



bool DataCache::load_from_nvs() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, using defaults");
        return false;
    }
    
    // Load stock data for all slots
    char key[16];
    for (int i = 0; i < 10; i++) {
        uint8_t valid = 0;
        snprintf(key, sizeof(key), "stk_valid_%d", i);
        if (nvs_get_u8(nvs, key, &valid) == ESP_OK) {
            stock_data_[i].valid = (valid != 0);
        }
        
        size_t len = sizeof(stock_data_[i].symbol);
        snprintf(key, sizeof(key), "stk_sym_%d", i);
        nvs_get_str(nvs, key, stock_data_[i].symbol, &len);
        
        double dbl;
        snprintf(key, sizeof(key), "stk_cur_%d", i);
        if (nvs_get_u64(nvs, key, (uint64_t*)&dbl) == ESP_OK) {
            stock_data_[i].current_price = dbl;
            snprintf(stock_data_[i].current_price_str, sizeof(stock_data_[i].current_price_str), "$%.2f", dbl);
        }
        
        snprintf(key, sizeof(key), "stk_prev_%d", i);
        if (nvs_get_u64(nvs, key, (uint64_t*)&dbl) == ESP_OK) {
            stock_data_[i].previous_close = dbl;
            snprintf(stock_data_[i].previous_close_str, sizeof(stock_data_[i].previous_close_str), "$%.2f", dbl);
        }
        
        snprintf(key, sizeof(key), "stk_chg_%d", i);
        if (nvs_get_u64(nvs, key, (uint64_t*)&dbl) == ESP_OK) {
            stock_data_[i].change_percent = dbl;
            snprintf(stock_data_[i].change_percent_str, sizeof(stock_data_[i].change_percent_str), "%+.2f%%", dbl);
        }
        
        snprintf(key, sizeof(key), "stk_hpre_%d", i);
        if (nvs_get_u8(nvs, key, &valid) == ESP_OK) {
            stock_data_[i].has_premarket = (valid != 0);
        }
        
        snprintf(key, sizeof(key), "stk_pprice_%d", i);
        if (nvs_get_u64(nvs, key, (uint64_t*)&dbl) == ESP_OK) {
            stock_data_[i].premarket_price = dbl;
            snprintf(stock_data_[i].premarket_price_str, sizeof(stock_data_[i].premarket_price_str), "$%.2f", dbl);
        }
        
        snprintf(key, sizeof(key), "stk_pchg_%d", i);
        if (nvs_get_u64(nvs, key, (uint64_t*)&dbl) == ESP_OK) {
            stock_data_[i].premarket_change_percent = dbl;
            snprintf(stock_data_[i].premarket_change_percent_str, sizeof(stock_data_[i].premarket_change_percent_str), 
                     "%+.2f%%", dbl);
        }

        // Load timestamp for this stock
        snprintf(key, sizeof(key), "ts_stk_%d", i);
        uint32_t ts = 0;
        if (nvs_get_u32(nvs, key, &ts) == ESP_OK) {
            ts_stock_last_fetch_[i] = ts;
        }
    }
    
    // Load weather data
    uint8_t valid;
    if (nvs_get_u8(nvs, NVS_KEY_WEATHER_VALID, &valid) == ESP_OK) {
        weather_data_.valid = (valid != 0);
    }
    
    size_t len = sizeof(weather_data_.temperature);
    nvs_get_str(nvs, NVS_KEY_WEATHER_TEMP, weather_data_.temperature, &len);
    
    len = sizeof(weather_data_.condition);
    nvs_get_str(nvs, NVS_KEY_WEATHER_COND, weather_data_.condition, &len);
    
    len = sizeof(weather_data_.humidity);
    nvs_get_str(nvs, NVS_KEY_WEATHER_HUM, weather_data_.humidity, &len);
    
    len = sizeof(weather_data_.rain_mm);
    nvs_get_str(nvs, NVS_KEY_WEATHER_RAIN, weather_data_.rain_mm, &len);

    // Load 15‑minute weather current data
    if (nvs_get_u8(nvs, NVS_KEY_WCUR_VALID, &valid) == ESP_OK) {
        weather_current_data_.valid = (valid != 0);
    }
    len = sizeof(weather_current_data_.temperature);
    nvs_get_str(nvs, NVS_KEY_WCUR_TEMP, weather_current_data_.temperature, &len);
    len = sizeof(weather_current_data_.condition);
    nvs_get_str(nvs, NVS_KEY_WCUR_COND, weather_current_data_.condition, &len);
    len = sizeof(weather_current_data_.humidity);
    nvs_get_str(nvs, NVS_KEY_WCUR_HUM, weather_current_data_.humidity, &len);
    len = sizeof(weather_current_data_.rain_mm);
    nvs_get_str(nvs, NVS_KEY_WCUR_RAIN, weather_current_data_.rain_mm, &len);
    
    // Load battery and market status
    nvs_get_u8(nvs, NVS_KEY_BATTERY, &battery_level_);

    if (nvs_get_u8(nvs, NVS_KEY_MARKET_OPEN, &valid) == ESP_OK) {
        market_open_ = (valid != 0);
    }
    
    // Load market close time
    uint32_t close_time_u32;
    if (nvs_get_u32(nvs, NVS_KEY_MARKET_CLOSE_TIME, &close_time_u32) == ESP_OK) {
        next_market_close_time_ = (time_t)close_time_u32;
    }

    // Load market open time
    uint32_t open_time_u32;
    if (nvs_get_u32(nvs, NVS_KEY_MARKET_OPEN_TIME, &open_time_u32) == ESP_OK) {
        next_market_open_time_ = (time_t)open_time_u32;
    }

    // Load global timestamps
    uint32_t u32;
    if (nvs_get_u32(nvs, NVS_KEY_TS_WEATHER, &u32) == ESP_OK) ts_weather_last_fetch_ = u32;
    if (nvs_get_u32(nvs, NVS_KEY_TS_MARKET, &u32) == ESP_OK) ts_market_last_check_ = u32;
    if (nvs_get_u32(nvs, NVS_KEY_TS_MARKET_OPEN_FETCH, &u32) == ESP_OK) ts_market_open_last_fetch_ = u32;
    
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Loaded cached data from NVS");
    return true;
}

bool DataCache::save_to_nvs() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return false;
    }
    
    // Save stock data for all slots
    char key[16];
    for (int i = 0; i < 10; i++) {
        snprintf(key, sizeof(key), "stk_valid_%d", i);
        nvs_set_u8(nvs, key, stock_data_[i].valid ? 1 : 0);
        
        snprintf(key, sizeof(key), "stk_sym_%d", i);
        nvs_set_str(nvs, key, stock_data_[i].symbol);
        
        snprintf(key, sizeof(key), "stk_cur_%d", i);
        nvs_set_u64(nvs, key, *(uint64_t*)&stock_data_[i].current_price);
        
        snprintf(key, sizeof(key), "stk_prev_%d", i);
        nvs_set_u64(nvs, key, *(uint64_t*)&stock_data_[i].previous_close);
        
        snprintf(key, sizeof(key), "stk_chg_%d", i);
        nvs_set_u64(nvs, key, *(uint64_t*)&stock_data_[i].change_percent);
        
        snprintf(key, sizeof(key), "stk_hpre_%d", i);
        nvs_set_u8(nvs, key, stock_data_[i].has_premarket ? 1 : 0);
        
        snprintf(key, sizeof(key), "stk_pprice_%d", i);
        nvs_set_u64(nvs, key, *(uint64_t*)&stock_data_[i].premarket_price);
        
        snprintf(key, sizeof(key), "stk_pchg_%d", i);
        nvs_set_u64(nvs, key, *(uint64_t*)&stock_data_[i].premarket_change_percent);

        // Save timestamp for this stock
        snprintf(key, sizeof(key), "ts_stk_%d", i);
        nvs_set_u32(nvs, key, ts_stock_last_fetch_[i]);
    }
    
    // Save weather data
    nvs_set_u8(nvs, NVS_KEY_WEATHER_VALID, weather_data_.valid ? 1 : 0);
    nvs_set_str(nvs, NVS_KEY_WEATHER_TEMP, weather_data_.temperature);
    nvs_set_str(nvs, NVS_KEY_WEATHER_COND, weather_data_.condition);
    nvs_set_str(nvs, NVS_KEY_WEATHER_HUM, weather_data_.humidity);
    nvs_set_str(nvs, NVS_KEY_WEATHER_RAIN, weather_data_.rain_mm);
    // Save 15‑minute current weather
    nvs_set_u8(nvs, NVS_KEY_WCUR_VALID, weather_current_data_.valid ? 1 : 0);
    nvs_set_str(nvs, NVS_KEY_WCUR_TEMP, weather_current_data_.temperature);
    nvs_set_str(nvs, NVS_KEY_WCUR_COND, weather_current_data_.condition);
    nvs_set_str(nvs, NVS_KEY_WCUR_HUM, weather_current_data_.humidity);
    nvs_set_str(nvs, NVS_KEY_WCUR_RAIN, weather_current_data_.rain_mm);
    
    // Save battery and market status
    nvs_set_u8(nvs, NVS_KEY_BATTERY, battery_level_);
    nvs_set_u8(nvs, NVS_KEY_MARKET_OPEN, market_open_ ? 1 : 0);
    nvs_set_u32(nvs, NVS_KEY_MARKET_CLOSE_TIME, (uint32_t)next_market_close_time_);
    nvs_set_u32(nvs, NVS_KEY_MARKET_OPEN_TIME, (uint32_t)next_market_open_time_);
    // Save global timestamps
    nvs_set_u32(nvs, NVS_KEY_TS_WEATHER, ts_weather_last_fetch_);
    nvs_set_u32(nvs, NVS_KEY_TS_MARKET, ts_market_last_check_);
    nvs_set_u32(nvs, NVS_KEY_TS_MARKET_OPEN_FETCH, ts_market_open_last_fetch_);
    
    err = nvs_commit(nvs);
    nvs_close(nvs);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved cached data to NVS");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return false;
    }
}

bool DataCache::clear_nvs_cache() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for cache clear: %s", esp_err_to_name(err));
        return false;
    }

    // Erase all keys in the namespace
    err = nvs_erase_all(nvs);

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        // Also invalidate in-memory cache by calling invalidate_all (which is already locked)
        // Note: We can't call invalidate_all here since we already hold the lock, so do it manually
        for (auto &stock : stock_data_) {
            memset(&stock, 0, sizeof(stock));
            stock.valid = false;
        }
        current_stock_index_ = 0;
        memset(&weather_data_, 0, sizeof(weather_data_));
        weather_data_.valid = false;
        memset(&weather_current_data_, 0, sizeof(weather_current_data_));
        weather_current_data_.valid = false;
        memset(&time_data_, 0, sizeof(time_data_));
        battery_level_ = 0;
        market_open_ = false;
        next_market_close_time_ = 0;
        next_market_open_time_ = 0;
        memset(ts_stock_last_fetch_, 0, sizeof(ts_stock_last_fetch_));
        ts_weather_last_fetch_ = 0;
        ts_market_last_check_ = 0;
        ts_market_open_last_fetch_ = 0;
        
        ESP_LOGI(TAG, "Cache cleared successfully from both NVS and memory");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to commit cache clear: %s", esp_err_to_name(err));
        return false;
    }
}
