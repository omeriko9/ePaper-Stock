#pragma once

#include "esp_log.h"
#include "adc_bsp.h"
#include "i2c_equipment.h"

// Stock data structure
typedef struct
{
    char symbol[16];
    double current_price;
    double previous_close;
    double change_percent;
    char current_price_str[32];
    char previous_close_str[32];
    char change_percent_str[32];
    // Premarket data
    double premarket_price;
    double premarket_change;
    double premarket_change_percent;
    char premarket_price_str[32];
    char premarket_change_str[32];
    char premarket_change_percent_str[32];
    bool has_premarket;
    bool valid;
} stock_data_t;

// Time data used for display (always in LOCAL timezone, not UTC)
// This struct represents the user's local time after timezone conversion.
typedef struct
{
    int year;        // Local year (e.g., 2025)
    int month;       // Local month (1-12)
    int day;         // Local day of month (1-31)
    int hour;        // Local hour (0-23) - ALREADY TIMEZONE ADJUSTED
    int minute;      // Local minute (0-59)
    int second;      // Local second (0-59)
    int week_day;    // Local day of week (0=Sun, 6=Sat)
    int day_of_year; // Local day of year (0-365)
    int moon_phase;  // Moon phase (not implemented)
} time_data_t;

typedef struct
{
    char location[64];
    char condition[64];
    char temperature[16];
    char humidity[16];
    char rain_mm[16];
    bool valid;
} weather_data_t;

typedef struct weather_settings_t
{

    char latitude[32];
    char longitude[32];
    char timezone[64]; // e.g. Asia/Jerusalem

} weather_settings_t;

class Utilities
{
public:
    // ========================================================================
    // TIME CONVERSION - SINGLE SOURCE OF TRUTH
    // ========================================================================
    // ARCHITECTURE OVERVIEW:
    // - RTC chip: stores UTC time only
    // - ESP32 system time (time_t): always UTC epoch (seconds since 1970-01-01 00:00:00 UTC)
    // - TZ environment variable: POSIX timezone string set from Settings (e.g., "EST5EDT,M3.2.0,M11.1.0")
    // - Conversion: Use ONLY these two functions below to convert between UTC and local time
    //
    // NAMING CONVENTION (MANDATORY):
    // - time_t variables: MUST end with "_utc" (e.g., system_time_utc, epoch_utc)
    // - struct tm variables: MUST be prefixed with "utc_" or "local_" (e.g., utc_tm, local_tm)
    // - RTC time variables: MUST be named "rtc_time_utc" (RTC stores UTC only)
    //
    // USAGE RULES:
    // 1. Never call setenv("TZ", ...) or localtime_r/mktime directly outside these functions
    // 2. Always use utc_epoch_to_local_tm() to convert system time for display
    // 3. Always use utc_tm_to_epoch() to convert RTC reads to system time
    // 4. All time comparisons use UTC epoch values
    // ========================================================================
    
    // Converts a UTC epoch timestamp to local time using the configured timezone.
    // This is the ONLY function that should apply timezone conversion for display.
    // Returns true on success, false if conversion fails.
    // Parameters:
    //   utc_epoch: UTC time as time_t (seconds since Unix epoch)
    //   local_tm_out: output struct tm in local timezone
    static bool utc_epoch_to_local_tm(time_t utc_epoch, struct tm *local_tm_out);
    
    // Converts UTC tm struct to epoch timestamp (inverse operation, for RTC reads)
    // This treats the input tm as UTC time and returns the epoch.
    // Returns 0 on error.
    static time_t utc_tm_to_epoch(const struct tm *utc_tm);

    static bool fetch_stock_data(const char *symbol, stock_data_t *data);
    static bool fetch_time_data(i2c_equipment *rtc_dev, time_data_t *data);
    static uint8_t fetch_battery_level();
    static bool fetch_weather_data(weather_data_t *data);
    // Fetch current conditions from Open‑Meteo current weather endpoint.
    static bool fetch_current_weather_data(weather_data_t *data);
    // Returns true if the market for the given symbol is currently open (REGULAR session)
    static bool is_us_market_open();
    // Local calculation of market open status (fallback when no internet/cache)
    static bool is_us_market_open_local();
    // Returns true if currently in extended hours (pre-market 4:00-9:30 ET or post-market 16:00-20:00 ET)
    static bool is_extended_hours_active();
    // Removed backwards-compatible/unused helpers to simplify codebase

    // Power/network helpers
    static bool is_on_battery_power();
    // Network helpers removed (unused)

    // Market timing data fetchers - SHOULD ONLY BE CALLED BY FETCH_TASK
    // These make HTTP requests and cache results in DataCache
    static time_t fetch_next_market_open_time();
    static time_t fetch_next_market_close_time();
    
    // Market timing helpers - safe for display/UI code (read from cache only)
    static int seconds_until_market_open();
    static int seconds_until_market_close();
    static void format_countdown_to_open(char *out, size_t out_sz);
    static void format_countdown_to_close(char *out, size_t out_sz);

    static constexpr char weather24hours_url[196] = "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&forecast_days=1&timezone=%s&hourly=temperature_2m,relative_humidity_2m,precipitation&temperature_unit=celsius&precipitation_unit=mm";
    static constexpr char weather_current_url[196] = "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,relative_humidity_2m,precipitation,weather_code&timezone=%s";

};
