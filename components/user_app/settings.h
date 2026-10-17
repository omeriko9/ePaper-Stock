#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mutex>

// Stock entry: symbol and alert thresholds
struct StockEntry {
    char symbol[16];
    float min_sound;  // Play sound when stock falls below this price (0 = disabled)
    float max_sound;  // Play sound when stock exceeds this price (0 = disabled)
};

// Centralized runtime configuration persisted in NVS.
// Keep it small and future-extensible.
struct SettingsData {
    uint16_t server_port;           // HTTP server port
    uint32_t sleep_interval_ms;     // Sleep interval between updates (when market open)
    uint32_t awake_after_update_ms; // How long to stay awake after refresh
    uint32_t market_closed_sleep_ms; // Sleep interval when market is closed (default 15 minutes)
    char     stock_symbol[16];      // DEPRECATED: kept for backward compat, use stocks[] instead
    char     weather_city[32];      // City for weather service
    char     timezone[64];          // TZ string (e.g., "EST5EDT,M3.2.0,M11.1.0")
    // Stock carousel (up to 10 stocks)
    StockEntry stocks[10];
    uint8_t    stock_count;         // Number of active stocks (0-10)
    // Weather (Open‑Meteo) settings
    char     weather_latitude[32];  // e.g. "40.7128"
    char     weather_longitude[32]; // e.g. "-74.0060"
    char     weather_tz[64];        // e.g. "America/New_York"
    // Fetch intervals (ms)
    uint32_t stock_fetch_interval_ms;        // Stock fetch interval (unused - now every wakeup)
    uint32_t device_sleep_interval_ms;       // Sleep chunk duration (default 500ms)
    bool     device_sleep_enabled;   // Whether to use esp_device_sleep_start
    bool     sound_enabled;         // Whether to play sounds
    float    stock_max_sound;       // DEPRECATED: use stocks[].max_sound
    float    stock_min_sound;       // DEPRECATED: use stocks[].min_sound
    uint8_t  audio_volume;          // Audio volume 0-100%
    uint32_t stock_carousel_interval_ms; // Time between stock switches when not sleeping (default 5000ms)
    bool     mock_mode_enabled;     // Use mock data instead of real API calls
    bool     log_capture_enabled;   // Enable log capture to /spiffs/log.txt (default: false)
    bool     dead_battery_image_enabled; // Show uploaded image when battery is below threshold
    // API tokens / credentials
    char     finnhub_token[96];     // Finnhub API token (X-Finnhub-Token header)
};

class Settings {
public:
    static Settings& instance();

    // Load from / Save to NVS. Returns true on success.
    bool load();
    bool save();

    // Accessors (thread-safe)
    SettingsData get();
    void update(const SettingsData& in);

    // Convenience getters
    uint16_t server_port();
    uint32_t sleep_interval_ms();
    uint32_t awake_after_update_ms();
    uint32_t market_closed_sleep_ms();
    uint32_t stock_fetch_interval_ms();  // Kept for backward compat
    uint32_t device_sleep_interval_ms();
    uint32_t stock_carousel_interval_ms();
    const char* stock_symbol();  // Returns first stock symbol for backward compat
    const StockEntry* stocks();  // Returns pointer to stocks array
    uint8_t stock_count();       // Returns number of active stocks
    const char* weather_city();
    const char* timezone();
    const char* weather_latitude();
    const char* weather_longitude();
    const char* weather_timezone();
    bool device_sleep_enabled();
    bool sound_enabled();
    float stock_max_sound();
    float stock_min_sound();
    uint8_t audio_volume();
    bool mock_mode_enabled();
    bool log_capture_enabled();
    bool dead_battery_image_enabled();
    const char* finnhub_token();

private:
    Settings();
    void set_defaults();
    bool ensure_nvs_open();

    SettingsData data_{};
    std::mutex mtx_;
    bool loaded_ = false;
};
