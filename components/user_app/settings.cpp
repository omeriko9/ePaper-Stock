#include "settings.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <string>

static const char* NVS_NS = "config";

Settings& Settings::instance() {
    static Settings s;
    return s;
}

Settings::Settings() {
    set_defaults();
}

void Settings::set_defaults() {
    data_.server_port = 9123;
    data_.sleep_interval_ms = 5000;         // keep current behavior
    data_.awake_after_update_ms = 5000;     // 5 seconds
    data_.market_closed_sleep_ms = 60 * 1000; // 15 * 60 * 1000; // 15 minutes (900000ms)
    strncpy(data_.stock_symbol, "INTC", sizeof(data_.stock_symbol));
    strncpy(data_.weather_city, "New York", sizeof(data_.weather_city));
    strncpy(data_.timezone, "EST5EDT,M3.2.0/2,M11.1.0/2", sizeof(data_.timezone));
    // Weather defaults for Open‑Meteo
    strncpy(data_.weather_latitude, "40.7128", sizeof(data_.weather_latitude));
    strncpy(data_.weather_longitude, "-74.0060", sizeof(data_.weather_longitude));
    strncpy(data_.weather_tz, "America/New_York", sizeof(data_.weather_tz));
    // Intervals defaults
    data_.stock_fetch_interval_ms = 60 * 1000;  // Kept for backward compat (unused)
    data_.device_sleep_interval_ms = 500;        // 500ms chunks
    data_.device_sleep_enabled = false;
    data_.sound_enabled = true;            // sounds disabled by default
    data_.stock_max_sound = 0.0f;           // 0 = disabled (deprecated)
    data_.stock_min_sound = 0.0f;           // 0 = disabled (deprecated)
    data_.audio_volume = 100;                // default 70% volume
    data_.stock_carousel_interval_ms = 5000; // 5 seconds between stocks
    data_.mock_mode_enabled = false;        // mock mode disabled by default
    data_.log_capture_enabled = false;      // log capture disabled by default
    data_.dead_battery_image_enabled = false;
    data_.finnhub_token[0] = '\0';         // empty by default
    // Initialize stock array with one default stock
    memset(data_.stocks, 0, sizeof(data_.stocks));
    strncpy(data_.stocks[0].symbol, "INTC", sizeof(data_.stocks[0].symbol));
    data_.stocks[0].min_sound = 0.0f;
    data_.stocks[0].max_sound = 0.0f;
    data_.stock_count = 1;
}

bool Settings::ensure_nvs_open() {
    // NVS is initialized in main already. Nothing to do here.
    return true;
}

bool Settings::load() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!ensure_nvs_open()) return false;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        // No data yet — keep defaults
        loaded_ = true;
        return true;
    }

    uint16_t u16temp;
    size_t len;

    if (nvs_get_u16(nvs, "port", &u16temp) == ESP_OK) data_.server_port = u16temp;
    uint32_t u32temp;
    if (nvs_get_u32(nvs, "sleep_ms", &u32temp) == ESP_OK) data_.sleep_interval_ms = u32temp;
    if (nvs_get_u32(nvs, "awake_ms", &u32temp) == ESP_OK) data_.awake_after_update_ms = u32temp;
    if (nvs_get_u32(nvs, "mkt_cls_ms", &u32temp) == ESP_OK) data_.market_closed_sleep_ms = u32temp;
    if (nvs_get_u32(nvs, "stk_int_ms", &u32temp) == ESP_OK) data_.stock_fetch_interval_ms = u32temp;
    if (nvs_get_u32(nvs, "ls_int_ms", &u32temp) == ESP_OK) data_.device_sleep_interval_ms = u32temp;

    len = sizeof(data_.stock_symbol);
    if (nvs_get_str(nvs, "stock", nullptr, &len) == ESP_OK && len <= sizeof(data_.stock_symbol)) {
        nvs_get_str(nvs, "stock", data_.stock_symbol, &len);
    }
    len = sizeof(data_.weather_city);
    if (nvs_get_str(nvs, "city", nullptr, &len) == ESP_OK && len <= sizeof(data_.weather_city)) {
        nvs_get_str(nvs, "city", data_.weather_city, &len);
    }
    len = sizeof(data_.timezone);
    if (nvs_get_str(nvs, "tz", nullptr, &len) == ESP_OK && len <= sizeof(data_.timezone)) {
        nvs_get_str(nvs, "tz", data_.timezone, &len);
    }
    len = sizeof(data_.weather_latitude);
    if (nvs_get_str(nvs, "w_lat", nullptr, &len) == ESP_OK && len <= sizeof(data_.weather_latitude)) {
        nvs_get_str(nvs, "w_lat", data_.weather_latitude, &len);
    }
    len = sizeof(data_.weather_longitude);
    if (nvs_get_str(nvs, "w_lon", nullptr, &len) == ESP_OK && len <= sizeof(data_.weather_longitude)) {
        nvs_get_str(nvs, "w_lon", data_.weather_longitude, &len);
    }
    len = sizeof(data_.weather_tz);
    if (nvs_get_str(nvs, "w_tz", nullptr, &len) == ESP_OK && len <= sizeof(data_.weather_tz)) {
        nvs_get_str(nvs, "w_tz", data_.weather_tz, &len);
    }
    // Finnhub token
    len = sizeof(data_.finnhub_token);
    if (nvs_get_str(nvs, "finnhub_token", nullptr, &len) == ESP_OK && len <= sizeof(data_.finnhub_token)) {
        nvs_get_str(nvs, "finnhub_token", data_.finnhub_token, &len);
    }
    uint8_t u8temp;
    if (nvs_get_u8(nvs, "dev_sleep", &u8temp) == ESP_OK) data_.device_sleep_enabled = (u8temp != 0);
    if (nvs_get_u8(nvs, "sound_en", &u8temp) == ESP_OK) data_.sound_enabled = (u8temp != 0);
    if (nvs_get_u8(nvs, "volume", &u8temp) == ESP_OK) data_.audio_volume = u8temp;
    if (nvs_get_u8(nvs, "mock_mode", &u8temp) == ESP_OK) data_.mock_mode_enabled = (u8temp != 0);
    if (nvs_get_u8(nvs, "log_cap", &u8temp) == ESP_OK) data_.log_capture_enabled = (u8temp != 0);
    if (nvs_get_u8(nvs, "dbat_img", &u8temp) == ESP_OK) data_.dead_battery_image_enabled = (u8temp != 0);
    
    // Load float values for stock thresholds
    if (nvs_get_u32(nvs, "snd_max", &u32temp) == ESP_OK) {
        memcpy(&data_.stock_max_sound, &u32temp, sizeof(float));
    }
    if (nvs_get_u32(nvs, "snd_min", &u32temp) == ESP_OK) {
        memcpy(&data_.stock_min_sound, &u32temp, sizeof(float));
    }
    
    // Load stock carousel interval
    if (nvs_get_u32(nvs, "stk_car_ms", &u32temp) == ESP_OK) data_.stock_carousel_interval_ms = u32temp;
    
    // Load stock array
    if (nvs_get_u8(nvs, "stk_cnt", &u8temp) == ESP_OK && u8temp <= 10) {
        data_.stock_count = u8temp;
        for (uint8_t i = 0; i < data_.stock_count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "stk%u_sym", i);
            len = sizeof(data_.stocks[i].symbol);
            if (nvs_get_str(nvs, key, nullptr, &len) == ESP_OK && len <= sizeof(data_.stocks[i].symbol)) {
                nvs_get_str(nvs, key, data_.stocks[i].symbol, &len);
            }
            snprintf(key, sizeof(key), "stk%u_min", i);
            if (nvs_get_u32(nvs, key, &u32temp) == ESP_OK) {
                memcpy(&data_.stocks[i].min_sound, &u32temp, sizeof(float));
            }
            snprintf(key, sizeof(key), "stk%u_max", i);
            if (nvs_get_u32(nvs, key, &u32temp) == ESP_OK) {
                memcpy(&data_.stocks[i].max_sound, &u32temp, sizeof(float));
            }
        }
    } else {
        // Migrate old single stock to new array if no stock_count found
        if (data_.stock_symbol[0] != '\0') {
            memset(data_.stocks, 0, sizeof(data_.stocks));
            strncpy(data_.stocks[0].symbol, data_.stock_symbol, sizeof(data_.stocks[0].symbol));
            data_.stocks[0].min_sound = data_.stock_min_sound;
            data_.stocks[0].max_sound = data_.stock_max_sound;
            data_.stock_count = 1;
        } else {
            data_.stock_count = 0;
        }
    }

    nvs_close(nvs);
    loaded_ = true;
    return true;
}

bool Settings::save() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!ensure_nvs_open()) return false;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return false;

    nvs_set_u16(nvs, "port", data_.server_port);
    nvs_set_u32(nvs, "sleep_ms", data_.sleep_interval_ms);
    nvs_set_u32(nvs, "awake_ms", data_.awake_after_update_ms);
    nvs_set_u32(nvs, "mkt_cls_ms", data_.market_closed_sleep_ms);
    nvs_set_u32(nvs, "stk_int_ms", data_.stock_fetch_interval_ms);
    nvs_set_u32(nvs, "ls_int_ms", data_.device_sleep_interval_ms);
    nvs_set_str(nvs, "stock", data_.stock_symbol);
    nvs_set_str(nvs, "city", data_.weather_city);
    nvs_set_str(nvs, "tz", data_.timezone);
    nvs_set_str(nvs, "w_lat", data_.weather_latitude);
    nvs_set_str(nvs, "w_lon", data_.weather_longitude);
    nvs_set_str(nvs, "w_tz", data_.weather_tz);
    // Finnhub token
    nvs_set_str(nvs, "finnhub_token", data_.finnhub_token);
    nvs_set_u8(nvs, "lsleep", data_.device_sleep_enabled ? 1 : 0);
    nvs_set_u8(nvs, "sound_en", data_.sound_enabled ? 1 : 0);
    nvs_set_u8(nvs, "volume", data_.audio_volume);
    nvs_set_u8(nvs, "mock_mode", data_.mock_mode_enabled ? 1 : 0);
    nvs_set_u8(nvs, "log_cap", data_.log_capture_enabled ? 1 : 0);
    nvs_set_u8(nvs, "dbat_img", data_.dead_battery_image_enabled ? 1 : 0);
    
    // Store float values as u32 (binary representation)
    uint32_t f_as_u32;
    memcpy(&f_as_u32, &data_.stock_max_sound, sizeof(float));
    nvs_set_u32(nvs, "snd_max", f_as_u32);
    memcpy(&f_as_u32, &data_.stock_min_sound, sizeof(float));
    nvs_set_u32(nvs, "snd_min", f_as_u32);
    
    // Store carousel interval
    nvs_set_u32(nvs, "stk_car_ms", data_.stock_carousel_interval_ms);
    
    // Store stock array
    nvs_set_u8(nvs, "stk_cnt", data_.stock_count);
    for (uint8_t i = 0; i < data_.stock_count && i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "stk%u_sym", i);
        nvs_set_str(nvs, key, data_.stocks[i].symbol);
        snprintf(key, sizeof(key), "stk%u_min", i);
        memcpy(&f_as_u32, &data_.stocks[i].min_sound, sizeof(float));
        nvs_set_u32(nvs, key, f_as_u32);
        snprintf(key, sizeof(key), "stk%u_max", i);
        memcpy(&f_as_u32, &data_.stocks[i].max_sound, sizeof(float));
        nvs_set_u32(nvs, key, f_as_u32);
    }
    
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    return (err == ESP_OK);
}

SettingsData Settings::get() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_;
}

void Settings::update(const SettingsData& in) {
    std::lock_guard<std::mutex> lock(mtx_);
    data_ = in;
}

uint16_t Settings::server_port() { return get().server_port; }
uint32_t Settings::sleep_interval_ms() { return get().sleep_interval_ms; }
uint32_t Settings::awake_after_update_ms() { return get().awake_after_update_ms; }
uint32_t Settings::market_closed_sleep_ms() { return get().market_closed_sleep_ms; }
uint32_t Settings::stock_fetch_interval_ms() { return get().stock_fetch_interval_ms; }
uint32_t Settings::device_sleep_interval_ms() { return get().device_sleep_interval_ms; }
uint32_t Settings::stock_carousel_interval_ms() { return get().stock_carousel_interval_ms; }
const char* Settings::stock_symbol() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    // Return first stock symbol for backward compatibility
    return (data_.stock_count > 0) ? data_.stocks[0].symbol : data_.stock_symbol;
}
const StockEntry* Settings::stocks() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_.stocks;
}
uint8_t Settings::stock_count() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_.stock_count;
}
const char* Settings::weather_city() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_.weather_city;
}
const char* Settings::timezone() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_.timezone;
}
const char* Settings::weather_latitude() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_.weather_latitude;
}
const char* Settings::weather_longitude() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_.weather_longitude;
}
const char* Settings::weather_timezone() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_.weather_tz;
}
bool Settings::device_sleep_enabled() { return get().device_sleep_enabled; }
bool Settings::sound_enabled() { return get().sound_enabled; }
float Settings::stock_max_sound() { return get().stock_max_sound; }
float Settings::stock_min_sound() { return get().stock_min_sound; }
uint8_t Settings::audio_volume() { return get().audio_volume; }
bool Settings::mock_mode_enabled() { return get().mock_mode_enabled; }
bool Settings::log_capture_enabled() { return get().log_capture_enabled; }
bool Settings::dead_battery_image_enabled() { return get().dead_battery_image_enabled; }
const char* Settings::finnhub_token() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!loaded_) load();
    return data_.finnhub_token;
}
