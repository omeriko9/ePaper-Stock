#include "utilities.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "settings.h"
#include "adc_bsp.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_spiffs.h"
#include "esp_attr.h"
#include "data_cache.h"


static const char *TAG_STOCK = "stock";
static const char *TAG_WEATHER = "weather";
static const char *TAG_MARKET = "market";
// Increase buffer to better accommodate larger JSON responses from wttr.in
// Place in PSRAM to avoid consuming internal DRAM (dram0) at link time
// Single shared HTTP response buffer (PSRAM) and bookkeeping
static constexpr size_t HTTP_BUFFER_SIZE = 32 * 1024; // 16 KB
static char *http_response_buffer = nullptr;
static int http_response_len = 0;
static volatile bool http_overflow = false;
static volatile size_t http_bytes_dropped = 0;

// Serialize temporary TZ changes to avoid cross-task races affecting localtime()
static SemaphoreHandle_t s_tz_mutex = nullptr;
static inline void tz_lock()
{
    if (!s_tz_mutex) {
        s_tz_mutex = xSemaphoreCreateMutex();
    }
    if (s_tz_mutex) {
        xSemaphoreTake(s_tz_mutex, portMAX_DELAY);
    }
}
static inline void tz_unlock()
{
    if (s_tz_mutex) {
        xSemaphoreGive(s_tz_mutex);
    }
}

// Allocate and free dynamic buffer safely
static void allocate_http_buffer() {
    if (!http_response_buffer) {
        http_response_buffer = reinterpret_cast<char*>(heap_caps_malloc(HTTP_BUFFER_SIZE, MALLOC_CAP_SPIRAM));
    }
    // Reset state for the next transfer
    http_response_len = 0;
    http_overflow = false;
    http_bytes_dropped = 0;
    if (http_response_buffer) {
        memset(http_response_buffer, 0, HTTP_BUFFER_SIZE);
    }
}

static void free_http_buffer() {
    if (http_response_buffer) {
        heap_caps_free(http_response_buffer);
        http_response_buffer = nullptr;
    }
}

// Finnhub token is now configurable via Settings (persisted in NVS)



// --- Helpers kept local to this file ----------------------------------------
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        if (!http_response_buffer) {
            // Should not happen if allocate_http_buffer() is called, but guard anyway
            return ESP_OK;
        }
        size_t space = (HTTP_BUFFER_SIZE > (size_t)http_response_len)
                        ? (HTTP_BUFFER_SIZE - (size_t)http_response_len)
                        : 0;
        size_t to_copy = (evt->data_len < space) ? evt->data_len : space;
        if (to_copy > 0) {
            memcpy(http_response_buffer + http_response_len, evt->data, to_copy);
            http_response_len += (int)to_copy;
        }
        if (to_copy < (size_t)evt->data_len) {
            http_overflow = true;
            http_bytes_dropped += ((size_t)evt->data_len - to_copy);
        }
    }
    return ESP_OK;
}
static bool http_get_into_buffer(const char* url, const char* token_header_key, const char* token_value);
static bool fetch_last_price_in_window(const char* symbol, time_t from_ts, time_t to_ts, double* out_last);
static void compute_et_now_and_windows(bool* is_weekend_now,
                                       bool* in_regular_now,
                                       time_t* t_pre_s,  time_t* t_pre_e,
                                       time_t* t_reg_s,  time_t* t_reg_e,
                                       time_t* t_post_s, time_t* t_post_e,
                                       time_t* y_post_s, time_t* y_post_e);
static bool is_open_meteo_timezone_valid(const char *timezone);
static void url_encode_component(const char *input, char *output, size_t output_size);
static void build_open_meteo_timezone(char *output, size_t output_size);
static void log_weather_http_failure(const char *url, int status_code);
static const char *weather_code_to_text(int weather_code);

static bool is_open_meteo_timezone_valid(const char *timezone)
{
    if (!timezone || !timezone[0]) {
        return false;
    }

    if (strcmp(timezone, "auto") == 0 || strcmp(timezone, "UTC") == 0 || strcmp(timezone, "GMT") == 0) {
        return true;
    }

    if (strchr(timezone, ',') != nullptr) {
        return false;
    }

    if (strchr(timezone, '/') == nullptr) {
        return false;
    }

    for (const char *cursor = timezone; *cursor; ++cursor) {
        unsigned char ch = static_cast<unsigned char>(*cursor);
        if (!(isalnum(ch) || ch == '/' || ch == '_' || ch == '-' || ch == '+')) {
            return false;
        }
    }

    return true;
}

static void url_encode_component(const char *input, char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return;
    }

    if (!input) {
        output[0] = '\0';
        return;
    }

    size_t out_index = 0;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(input); *cursor && out_index + 1 < output_size; ++cursor) {
        if (isalnum(*cursor) || *cursor == '-' || *cursor == '_' || *cursor == '.' || *cursor == '~') {
            output[out_index++] = static_cast<char>(*cursor);
            continue;
        }

        if (out_index + 3 >= output_size) {
            break;
        }

        snprintf(output + out_index, output_size - out_index, "%%%02X", *cursor);
        out_index += 3;
    }

    output[out_index] = '\0';
}

static void build_open_meteo_timezone(char *output, size_t output_size)
{
    char sanitized[64] = {0};
    const char *configured_timezone = Settings::instance().weather_timezone();

    if (is_open_meteo_timezone_valid(configured_timezone)) {
        strncpy(sanitized, configured_timezone, sizeof(sanitized) - 1);
    } else {
        strncpy(sanitized, "auto", sizeof(sanitized) - 1);
        ESP_LOGW(TAG_WEATHER, "Invalid weather timezone '%s' - using Open-Meteo auto timezone resolution", configured_timezone ? configured_timezone : "<null>");
    }

    url_encode_component(sanitized, output, output_size);
}

static void log_weather_http_failure(const char *url, int status_code)
{
    ESP_LOGE(TAG_WEATHER, "Bad status %d len %d url=%s", status_code, http_response_len, url ? url : "<null>");
    if (http_response_len > 0 && http_response_buffer) {
        char debug_body[161];
        int copy_len = http_response_len > 160 ? 160 : http_response_len;
        memcpy(debug_body, http_response_buffer, copy_len);
        debug_body[copy_len] = '\0';
        ESP_LOGE(TAG_WEATHER, "Response body: %s", debug_body);
    }
}

static const char *weather_code_to_text(int weather_code)
{
    switch (weather_code) {
        case 0: return "Clear";
        case 1:
        case 2:
        case 3: return "Cloudy";
        case 45:
        case 48: return "Fog";
        case 51:
        case 53:
        case 55:
        case 56:
        case 57: return "Drizzle";
        case 61:
        case 63:
        case 65:
        case 66:
        case 67:
        case 80:
        case 81:
        case 82: return "Rain";
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86: return "Snow";
        case 95:
        case 96:
        case 99: return "Storm";
        default: return "Weather";
    }
}

static void generate_mock_stock_data(const char *symbol, stock_data_t *data)
{
    static uint32_t seed = 0;
    if (seed == 0) {
        seed = (uint32_t)time(NULL);
    }
    
    seed = seed * 1103515245 + 12345;
    double base_price = 50.0 + (double)((seed >> 16) % 200);
    
    seed = seed * 1103515245 + 12345;
    double price_variation = ((double)((seed >> 16) % 2000) - 1000.0) / 100.0;
    
    data->current_price = base_price + price_variation;
    data->previous_close = base_price;
    data->change_percent = (data->current_price - data->previous_close) / data->previous_close * 100.0;
    
    snprintf(data->current_price_str, sizeof(data->current_price_str), "$%.2f", data->current_price);
    snprintf(data->previous_close_str, sizeof(data->previous_close_str), "$%.2f", data->previous_close);
    snprintf(data->change_percent_str, sizeof(data->change_percent_str), "%+.2f%%", data->change_percent);
    
    seed = seed * 1103515245 + 12345;
    bool has_premarket = ((seed >> 16) % 2) == 0;
    
    if (has_premarket) {
        seed = seed * 1103515245 + 12345;
        double premarket_variation = ((double)((seed >> 16) % 1000) - 500.0) / 100.0;
        data->premarket_price = data->previous_close + premarket_variation;
        data->premarket_change = data->premarket_price - data->previous_close;
        data->premarket_change_percent = (data->premarket_change / data->previous_close) * 100.0;
        
        snprintf(data->premarket_price_str, sizeof(data->premarket_price_str), "$%.2f", data->premarket_price);
        snprintf(data->premarket_change_str, sizeof(data->premarket_change_str), "%+.2f", data->premarket_change);
        snprintf(data->premarket_change_percent_str, sizeof(data->premarket_change_percent_str), "%+.2f%%", data->premarket_change_percent);
        data->has_premarket = true;
    } else {
        data->has_premarket = false;
    }
    
    strncpy(data->symbol, symbol, sizeof(data->symbol) - 1);
    data->symbol[sizeof(data->symbol) - 1] = '\0';
    data->valid = true;
}

#include "esp_task_wdt.h"
// --- Implementation ----------------------------------------------------------
bool Utilities::fetch_stock_data(const char *symbol, stock_data_t *data)
{
    if (!symbol || !data) return false;

    if (Settings::instance().mock_mode_enabled()) {
        ESP_LOGI(TAG_STOCK, "Mock mode enabled - generating random stock data for %s", symbol);
        generate_mock_stock_data(symbol, data);
        return true;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ESP_LOGW(TAG_STOCK, "WiFi not connected - cannot fetch data for %s", symbol);
        data->valid = false;
        return false;
    }

    // --- Log request URL and token visibility for debugging ---
    char url[256];
    snprintf(url, sizeof(url), "https://finnhub.io/api/v1/quote?symbol=%s", symbol);
    ESP_LOGI(TAG_STOCK, "Preparing HTTP GET: %s", url);
    const char* finnhub_token = Settings::instance().finnhub_token();
    if (finnhub_token && strlen(finnhub_token) > 0)
        ESP_LOGI(TAG_STOCK, "Using Finnhub token (length=%d)", (int)strlen(finnhub_token));
    else
        ESP_LOGW(TAG_STOCK, "No Finnhub token configured!");

    // Ensure buffer ready before HTTP
    allocate_http_buffer();
    if (!http_response_buffer) {
        ESP_LOGE(TAG_STOCK, "HTTP buffer allocation failed for %s", symbol);
        data->valid = false;
        return false;
    }

    // --- Perform blocking HTTP GET safely, allowing WDT feed ---
    esp_task_wdt_reset();
    bool ok = http_get_into_buffer(url, "X-Finnhub-Token", finnhub_token);
    esp_task_wdt_reset();
    if (!ok) {
        ESP_LOGE(TAG_STOCK, "HTTP GET failed for %s (URL=%s)", symbol, url);
        data->valid = false;
        free_http_buffer();
        return false;
    }

    cJSON *root = cJSON_Parse(http_response_buffer);
    free_http_buffer();
    if (!root) {
        ESP_LOGE(TAG_STOCK, "Failed to parse JSON response for %s", symbol);
        data->valid = false;
        return false;
    }

    cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "c");
    cJSON *jpc = cJSON_GetObjectItemCaseSensitive(root, "pc");
    cJSON *jdp = cJSON_GetObjectItemCaseSensitive(root, "dp");
    if (!cJSON_IsNumber(jc) || !cJSON_IsNumber(jpc)) {
        ESP_LOGE(TAG_STOCK, "Missing numeric fields in JSON for %s", symbol);
        cJSON_Delete(root);
        data->valid = false;
        return false;
    }

    data->current_price = jc->valuedouble;
    data->previous_close = jpc->valuedouble;
    data->change_percent =
        (cJSON_IsNumber(jdp)
             ? jdp->valuedouble
             : (data->previous_close
                    ? ((data->current_price - data->previous_close) / data->previous_close * 100.0)
                    : 0.0));
    snprintf(data->current_price_str, sizeof(data->current_price_str), "$%.2f", data->current_price);
    snprintf(data->previous_close_str, sizeof(data->previous_close_str), "$%.2f", data->previous_close);
    snprintf(data->change_percent_str, sizeof(data->change_percent_str), "%+.2f%%", data->change_percent);

    cJSON_Delete(root);
    esp_task_wdt_reset();  // feed watchdog at completion

    // 2) Extended-hours logic (what to show when REGULAR is closed)
    bool weekend=false, in_regular=false;
    time_t pre_s=0, pre_e=0, reg_s=0, reg_e=0, post_s=0, post_e=0, ypost_s=0, ypost_e=0;
    compute_et_now_and_windows(&weekend, &in_regular,
                               &pre_s, &pre_e, &reg_s, &reg_e, &post_s, &post_e, &ypost_s, &ypost_e);

    data->has_premarket = false;
    time_t now_ts = time(NULL);
    
    // Debug logging for market windows
    ESP_LOGI(TAG_STOCK, "[PREMARKET] Current time (UTC): %ld", (long)now_ts);
    ESP_LOGI(TAG_STOCK, "[PREMARKET] Weekend: %s, In Regular Hours: %s", 
             weekend ? "YES" : "NO", in_regular ? "YES" : "NO");
    ESP_LOGI(TAG_STOCK, "[PREMARKET] Today's PRE window: %ld to %ld (4:00-9:30 ET)", (long)pre_s, (long)pre_e);
    ESP_LOGI(TAG_STOCK, "[PREMARKET] Today's REG window: %ld to %ld (9:30-16:00 ET)", (long)reg_s, (long)reg_e);
    ESP_LOGI(TAG_STOCK, "[PREMARKET] Today's POST window: %ld to %ld (16:00-20:00 ET)", (long)post_s, (long)post_e);
    ESP_LOGI(TAG_STOCK, "[PREMARKET] Yesterday's POST window: %ld to %ld", (long)ypost_s, (long)ypost_e);

    if (!in_regular) {
        double ext_price = 0.0;
        bool got = false;
        const char* window_name = "unknown";

        if (difftime(now_ts, pre_s) >= 0 && difftime(pre_e, now_ts) > 0) {
            // In today's PRE
            window_name = "PRE-MARKET";
            ESP_LOGI(TAG_STOCK, "[PREMARKET] Currently in PRE-MARKET window, fetching extended hours data...");
            got = fetch_last_price_in_window(symbol, pre_s, now_ts, &ext_price);  // Use now_ts as end, not pre_e
        } else if (difftime(now_ts, reg_e) >= 0 && difftime(post_e, now_ts) > 0) {
            // In today's POST
            window_name = "POST-MARKET";
            ESP_LOGI(TAG_STOCK, "[PREMARKET] Currently in POST-MARKET window, fetching extended hours data...");
            got = fetch_last_price_in_window(symbol, post_s, now_ts, &ext_price);  // Use now_ts as end, not post_e
        } else {
            // Overnight gap or weekend/holiday -> use yesterday POST if available
            window_name = "OVERNIGHT/WEEKEND";
            ESP_LOGI(TAG_STOCK, "[PREMARKET] Currently in OVERNIGHT/WEEKEND gap, fetching yesterday's post-market...");
            got = fetch_last_price_in_window(symbol, ypost_s, ypost_e, &ext_price);
        }

        if (got) {
            data->premarket_price = ext_price;
            data->premarket_change = data->premarket_price - data->previous_close;
            data->premarket_change_percent =
                (data->previous_close ? (data->premarket_change / data->previous_close * 100.0) : 0.0);

            snprintf(data->premarket_price_str, sizeof(data->premarket_price_str), "$%.2f", data->premarket_price);
            snprintf(data->premarket_change_str, sizeof(data->premarket_change_str), "%+.2f", data->premarket_change);
            snprintf(data->premarket_change_percent_str, sizeof(data->premarket_change_percent_str), "%+.2f%%",
                     data->premarket_change_percent);

            data->has_premarket = true;
            ESP_LOGI(TAG_STOCK, "[PREMARKET] Got extended hours price for %s: %s (%s) in %s window",
                     symbol, data->premarket_price_str, data->premarket_change_percent_str, window_name);
        } else {
            // No extended-hours data available now — keep has_premarket=false.
            data->has_premarket = false;
            ESP_LOGW(TAG_STOCK, "[PREMARKET] No extended hours data available for %s in %s window", symbol, window_name);
        }
    } else {
        ESP_LOGI(TAG_STOCK, "[PREMARKET] Market is in REGULAR hours, skipping extended-hours fetch");
    }

    // finalize
    strncpy(data->symbol, symbol, sizeof(data->symbol)-1);
    data->symbol[sizeof(data->symbol)-1] = '\0';
    data->valid = true;
    return true;
}

// ===================== helpers =====================

static bool http_get_into_buffer(const char* url, const char* token_header_key, const char* token_value) {
    // Ensure buffer is allocated and cleared
    allocate_http_buffer();
    if (!http_response_buffer) {
        return false;
    }
    http_response_len = 0;
    http_response_buffer[0] = '\0';
    http_overflow = false;
    http_bytes_dropped = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event_handler;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    esp_http_client_set_header(cli, "User-Agent", "ESP32/1.0 (ePaper-Stock)");
    esp_http_client_set_header(cli, "Accept", "application/json");
    if (token_header_key && token_value) esp_http_client_set_header(cli, token_header_key, token_value);

    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_STOCK, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return false;
    }

    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || http_response_len <= 0) {
        ESP_LOGE(TAG_STOCK, "Bad HTTP status=%d len=%d (overflow=%d dropped=%u)", status, http_response_len, http_overflow, (unsigned)http_bytes_dropped);
        // Log first 200 chars of response for debugging
        if (http_response_len > 0) {
            char debug_buf[201];
            int copy_len = http_response_len > 200 ? 200 : http_response_len;
            strncpy(debug_buf, http_response_buffer, copy_len);
            debug_buf[copy_len] = '\0';
            ESP_LOGE(TAG_STOCK, "Response body: %s", debug_buf);
        }
        esp_http_client_cleanup(cli);
        return false;
    }

    http_response_buffer[http_response_len] = '\0';
    if (http_overflow) {
        ESP_LOGW(TAG_STOCK, "HTTP buffer overflow: truncated. Dropped=%u, stored=%d, buf=%u",
                 (unsigned)http_bytes_dropped, http_response_len, (unsigned)HTTP_BUFFER_SIZE);
    }
    esp_http_client_cleanup(cli);
    return true;
}

static bool fetch_last_price_in_window(const char* symbol, time_t from_ts, time_t to_ts, double* out_last) {
    if (from_ts <= 0 || to_ts <= 0 || from_ts >= to_ts) {
        ESP_LOGW(TAG_STOCK, "[PREMARKET] Invalid time window: from=%ld, to=%ld", (long)from_ts, (long)to_ts);
        return false;
    }

    ESP_LOGI(TAG_STOCK, "[PREMARKET] Fetching candle data for %s from %ld to %ld", symbol, (long)from_ts, (long)to_ts);

    allocate_http_buffer();
    char url[256];
    snprintf(url, sizeof(url),
             "https://finnhub.io/api/v1/stock/candle?symbol=%s&resolution=1&from=%ld&to=%ld",
             symbol, (long)from_ts, (long)to_ts);
    
    ESP_LOGI(TAG_STOCK, "[PREMARKET] Candle URL: %s", url);

    const char* finnhub_token2 = Settings::instance().finnhub_token();
    if (!http_get_into_buffer(url, "X-Finnhub-Token", finnhub_token2)) {
        ESP_LOGE(TAG_STOCK, "[PREMARKET] HTTP GET failed for candle data");
        return false;
    }

    cJSON* j = cJSON_Parse(http_response_buffer);
    if (!j) {
        ESP_LOGE(TAG_STOCK, "[PREMARKET] Failed to parse candle JSON response");
        return false;
    }

    bool ok = false;
    cJSON* s  = cJSON_GetObjectItemCaseSensitive(j, "s");
    cJSON* cc = cJSON_GetObjectItemCaseSensitive(j, "c");
    
    if (cJSON_IsString(s)) {
        ESP_LOGI(TAG_STOCK, "[PREMARKET] Candle response status: %s", s->valuestring);
        
        if (strcmp(s->valuestring, "ok") == 0 && cJSON_IsArray(cc) && cJSON_GetArraySize(cc) > 0) {
            int num_candles = cJSON_GetArraySize(cc);
            ESP_LOGI(TAG_STOCK, "[PREMARKET] Got %d candles, using last one", num_candles);
            cJSON* last_close = cJSON_GetArrayItem(cc, num_candles - 1);
            if (cJSON_IsNumber(last_close)) {
                *out_last = last_close->valuedouble;
                ESP_LOGI(TAG_STOCK, "[PREMARKET] Last price in window: $%.2f", *out_last);
                ok = true;
            }
        } else if (strcmp(s->valuestring, "no_data") == 0) {
            ESP_LOGW(TAG_STOCK, "[PREMARKET] No candle data available for this time window (status=no_data)");
        } else {
            ESP_LOGW(TAG_STOCK, "[PREMARKET] Unexpected candle status: %s", s->valuestring);
        }
    } else {
        ESP_LOGW(TAG_STOCK, "[PREMARKET] Candle response missing 's' field");
    }
    
    cJSON_Delete(j);
    return ok;
}

static void compute_et_now_and_windows(bool* is_weekend_now,
                                       bool* in_regular_now,
                                       time_t* t_pre_s,  time_t* t_pre_e,
                                       time_t* t_reg_s,  time_t* t_reg_e,
                                       time_t* t_post_s, time_t* t_post_e,
                                       time_t* y_post_s, time_t* y_post_e)
{
    // DO NOT change global TZ - use UTC and calculate ET offset manually
    // EST = UTC-5, EDT = UTC-4 (daylight saving time)
    // DST in US: 2nd Sunday of March 2am to 1st Sunday of November 2am
    // This function is called from fetch task context, which has watchdog enabled
    
    time_t now = time(NULL);
    struct tm utc_now;
    gmtime_r(&now, &utc_now);
    
    ESP_LOGD(TAG_STOCK, "[ET_CALC] UTC now: %04d-%02d-%02d %02d:%02d:%02d (wday=%d)",
             utc_now.tm_year + 1900, utc_now.tm_mon + 1, utc_now.tm_mday,
             utc_now.tm_hour, utc_now.tm_min, utc_now.tm_sec, utc_now.tm_wday);
    
    // Convert UTC to ET by subtracting 5 hours (EST) or 4 hours (EDT)
    // DST in US: 2nd Sunday of March 2am to 1st Sunday of November 2am
    // More accurate DST calculation using day-of-week
    auto is_dst = [](int year, int month, int day, int wday) -> bool {
        // DST starts: 2nd Sunday of March at 2:00 AM
        // DST ends: 1st Sunday of November at 2:00 AM
        if (month < 3 || month > 11) return false;  // Jan, Feb, Dec: EST
        if (month > 3 && month < 11) return true;   // Apr-Oct: EDT
        
        // March: DST starts on 2nd Sunday
        if (month == 3) {
            // Calculate day of 2nd Sunday: first Sunday + 7
            // First Sunday = 1 + (7 - wday_of_day_1) % 7
            // We need to find what wday was on day 1 of this month
            int wday_offset = (day - 1) % 7;
            int wday_of_day1 = (wday - wday_offset + 7) % 7;
            int first_sunday = (wday_of_day1 == 0) ? 1 : (8 - wday_of_day1);
            int second_sunday = first_sunday + 7;
            return day >= second_sunday;
        }
        
        // November: DST ends on 1st Sunday
        if (month == 11) {
            int wday_offset = (day - 1) % 7;
            int wday_of_day1 = (wday - wday_offset + 7) % 7;
            int first_sunday = (wday_of_day1 == 0) ? 1 : (8 - wday_of_day1);
            return day < first_sunday;
        }
        
        return false;
    };
    
    int et_offset_hours = is_dst(utc_now.tm_year + 1900, utc_now.tm_mon + 1, utc_now.tm_mday, utc_now.tm_wday) ? -4 : -5;
    ESP_LOGD(TAG_STOCK, "[ET_CALC] ET offset: %d hours (DST=%s)", 
             et_offset_hours, (et_offset_hours == -4) ? "YES" : "NO");
    
    // Create ET time by applying offset to current UTC time
    time_t et_time = now + (et_offset_hours * 3600);
    struct tm et_now;
    gmtime_r(&et_time, &et_now);

    // helper to build a specific ET time today - returns UTC timestamp
    // OPTIMIZED: Use mktime with UTC calculation instead of year-by-year loops
    auto at_time = [&et_now, et_offset_hours](int hh, int mm, int ss) -> time_t {
        struct tm base = et_now;
        base.tm_hour = hh; 
        base.tm_min = mm; 
        base.tm_sec = ss;
        base.tm_isdst = 0;
        
        // Calculate epoch time efficiently using a mathematical formula
        // Days since epoch calculation (optimized - no loops)
        int year = base.tm_year + 1900;
        int month = base.tm_mon + 1;
        int day = base.tm_mday;
        
        // Calculate days since 1970-01-01 using formula (much faster than looping)
        int y = year - 1970;
        int leap_years = (y + 1) / 4 - (y + 69) / 100 + (y + 369) / 400;
        int days = y * 365 + leap_years;
        
        // Add days for months
        const int days_before_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        days += days_before_month[month - 1];
        
        // Add leap day if this year is leap and month > February
        bool is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (is_leap && month > 2) days++;
        
        // Add day of month
        days += day - 1;
        
        time_t result = (time_t)days * 86400LL + base.tm_hour * 3600LL + base.tm_min * 60LL + base.tm_sec;
        
        // This is ET time, subtract offset to get UTC
        return result - (et_offset_hours * 3600);
    };

    time_t pre_s  = at_time(4,  0, 0);
    time_t pre_e  = at_time(9, 30, 0);
    time_t reg_s  = at_time(9, 30, 0);
    time_t reg_e  = at_time(16,  0, 0);
    time_t post_s = at_time(16,  0, 0);
    time_t post_e = at_time(20,  0, 0);

    bool weekend = (et_now.tm_wday == 0 || et_now.tm_wday == 6);
    bool in_reg  = (!weekend) && (difftime(now, reg_s) >= 0) && (difftime(reg_e, now) > 0);

    // OPTIMIZED: Calculate previous business day directly without loop
    // Subtract days based on current day of week to avoid iterative date normalization
    int days_back = 1;
    if (et_now.tm_wday == 0) days_back = 2;      // Sunday -> Friday
    else if (et_now.tm_wday == 1) days_back = 3; // Monday -> Friday
    
    // Calculate previous business day time directly
    time_t prev_time = et_time - (days_back * 86400);
    struct tm prev;
    gmtime_r(&prev_time, &prev);
    
    // OPTIMIZED: Use efficient formula instead of year-by-year loops
    auto at_time_prev = [&prev, et_offset_hours](int hh, int mm, int ss) -> time_t {
        struct tm base = prev;
        base.tm_hour = hh; 
        base.tm_min = mm; 
        base.tm_sec = ss;
        base.tm_isdst = 0;
        
        // Calculate epoch time efficiently
        int year = base.tm_year + 1900;
        int month = base.tm_mon + 1;
        int day = base.tm_mday;
        
        // Calculate days since 1970-01-01 using formula (much faster)
        int y = year - 1970;
        int leap_years = (y + 1) / 4 - (y + 69) / 100 + (y + 369) / 400;
        int days = y * 365 + leap_years;
        
        // Add days for months
        const int days_before_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        days += days_before_month[month - 1];
        
        // Add leap day if this year is leap and month > February
        bool is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (is_leap && month > 2) days++;
        
        // Add day of month
        days += day - 1;
        
        time_t result = (time_t)days * 86400LL + base.tm_hour * 3600LL + base.tm_min * 60LL + base.tm_sec;
        return result - (et_offset_hours * 3600);
    };
    
    time_t y_postS = at_time_prev(16, 0, 0);
    time_t y_postE = at_time_prev(20, 0, 0);

    if (is_weekend_now) *is_weekend_now = weekend;
    if (in_regular_now) *in_regular_now = in_reg;
    if (t_pre_s)  *t_pre_s  = pre_s;
    if (t_pre_e)  *t_pre_e  = pre_e;
    if (t_reg_s)  *t_reg_s  = reg_s;
    if (t_reg_e)  *t_reg_e  = reg_e;
    if (t_post_s) *t_post_s = post_s;
    if (t_post_e) *t_post_e = post_e;
    if (y_post_s) *y_post_s = y_postS;
    if (y_post_e) *y_post_e = y_postE;

    // NO TZ RESTORE NEEDED - we never changed it!
}

// Replace your Yahoo-based function with this.
// Returns true only during REGULAR session. Optionally returns the session string.
bool Utilities::is_us_market_open()
{
    // Track last fetch time and failures to avoid rapid retries on DNS errors
    static time_t s_last_fetch_attempt = 0;
    static esp_err_t s_last_error = ESP_OK;
    static bool s_last_open_state = false; // Persist last known state; default to closed on cold boot
    const time_t RETRY_DELAY_ON_ERROR = 300;   // Wait 5 minutes before retrying after error
    const time_t RETRY_DELAY_ON_SUCCESS = 600; // Wait 10 minutes between successful checks

    time_t now = time(NULL);

    // Avoid hammering the API on repeated failures
    if (s_last_error != ESP_OK && (now - s_last_fetch_attempt) < RETRY_DELAY_ON_ERROR) {
        ESP_LOGD(TAG_MARKET, "Skipping market status check - last attempt failed %ld seconds ago", now - s_last_fetch_attempt);
        // IMPORTANT: Do NOT report 'closed' just because we skipped. Return last known state.
        return s_last_open_state;
    }

    // Also avoid unnecessary checks when connection is stable
    if (s_last_error == ESP_OK && (now - s_last_fetch_attempt) < RETRY_DELAY_ON_SUCCESS) {
        ESP_LOGD(TAG_MARKET, "Skipping market status check - last successful check was %ld seconds ago", now - s_last_fetch_attempt);
        // IMPORTANT: Preserve state instead of forcing 'closed'.
        return s_last_open_state;
    }

    s_last_fetch_attempt = now;

    const char*  exchange = "US";

    // --- HTTP setup
    allocate_http_buffer();
    if (!http_response_buffer) { 
        ESP_LOGE(TAG_MARKET, "HTTP buffer alloc failed"); 
        s_last_error = ESP_ERR_NO_MEM;
        return false; 
    }
    http_response_len = 0;
    memset(http_response_buffer, 0, HTTP_BUFFER_SIZE);

    char url[256];
    snprintf(url, sizeof(url),
             "https://finnhub.io/api/v1/stock/market-status?exchange=%s", exchange);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 8000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_event_handler;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG_MARKET, "HTTP init failed");
        s_last_error = ESP_ERR_NO_MEM;
        return s_last_open_state; // Keep previous state on failure
    }

    // Prefer header over query for the token (both are supported)
    esp_http_client_set_header(cli, "X-Finnhub-Token", Settings::instance().finnhub_token());
    esp_http_client_set_header(cli, "Accept", "application/json");
    esp_http_client_set_header(cli, "User-Agent", "ESP32/1.0 (ePaper-Stock)");

    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MARKET, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        s_last_error = err;
        return s_last_open_state; // Keep previous state on failure
    }
    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || http_response_len <= 0) {
        ESP_LOGE(TAG_MARKET, "Bad HTTP status=%d len=%d", status, http_response_len);
        esp_http_client_cleanup(cli);
        s_last_error = ESP_ERR_INVALID_RESPONSE;
        return s_last_open_state; // Keep previous state on failure
    }
    http_response_buffer[http_response_len] = '\0';
    esp_http_client_cleanup(cli);

    // --- Parse
    bool open_regular = false;
    const char* session = nullptr;

    cJSON* root = cJSON_Parse(http_response_buffer);
    if (root) {
        // Finnhub returns JSON indicating open/closed and a session (e.g., pre/regular/post).
        // Exact field names may evolve; handle both typical patterns.
        cJSON* isOpen = cJSON_GetObjectItemCaseSensitive(root, "isOpen");
        cJSON* sess   = cJSON_GetObjectItemCaseSensitive(root, "session");
        if (cJSON_IsString(sess)) session = sess->valuestring;

        if (cJSON_IsBool(isOpen)) {
            // Treat "open" + session==regular as market open for trading.
            if (cJSON_IsTrue(isOpen) && session && strcasecmp(session, "regular") == 0) {
                open_regular = true;
            }
        } else if (session) {
            // Fallback: decide purely by session text
            open_regular = (strcasecmp(session, "regular") == 0);
        }
        // if (session_out && session_out_sz) {
        //     snprintf(session_out, session_out_sz, "%s", session ? session : "unknown");
        // }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG_MARKET, "JSON parse failed");
    }
    
    // Mark successful fetch and remember state
    s_last_error = ESP_OK;
    s_last_open_state = open_regular;
    return open_regular;
}

// Removed legacy is_market_open_old()

// Local calculation of market open status (fallback when no internet/cache)
bool Utilities::is_us_market_open_local()
{
    tz_lock();
    
    // Set TZ to ET
    const char* old_tz = getenv("TZ");
    char old_tz_copy[64] = {0};
    if (old_tz) {
        strncpy(old_tz_copy, old_tz, sizeof(old_tz_copy) - 1);
    }
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    
    time_t now = time(NULL);
    struct tm et_now;
    localtime_r(&now, &et_now);
    
    // Market hours: 9:30 AM - 4:00 PM ET, weekdays only
    bool is_weekday = (et_now.tm_wday >= 1 && et_now.tm_wday <= 5);
    bool in_hours = (et_now.tm_hour > 9 || (et_now.tm_hour == 9 && et_now.tm_min >= 30)) &&
                    (et_now.tm_hour < 16 || (et_now.tm_hour == 16 && et_now.tm_min == 0));
    
    bool open = is_weekday && in_hours;
    
    // Restore TZ
    if (old_tz_copy[0]) {
        setenv("TZ", old_tz_copy, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    tz_unlock();
    
    ESP_LOGD(TAG_MARKET, "Local market check: ET %02d:%02d wday=%d, weekday=%d, in_hours=%d, open=%d",
             et_now.tm_hour, et_now.tm_min, et_now.tm_wday, is_weekday, in_hours, open);
    
    return open;
}

// Returns true if currently in extended hours (pre-market or post-market) on a weekday
// Pre-market: 4:00 AM - 9:30 AM ET
// Post-market: 4:00 PM - 8:00 PM ET
bool Utilities::is_extended_hours_active()
{
    tz_lock();
    
    // Set TZ to ET
    const char* old_tz = getenv("TZ");
    char old_tz_copy[64] = {0};
    if (old_tz) {
        strncpy(old_tz_copy, old_tz, sizeof(old_tz_copy) - 1);
    }
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    
    time_t now = time(NULL);
    struct tm et_now;
    localtime_r(&now, &et_now);
    
    // Extended hours only on weekdays
    bool is_weekday = (et_now.tm_wday >= 1 && et_now.tm_wday <= 5);
    
    // Pre-market: 4:00 AM - 9:30 AM ET
    bool in_premarket = (et_now.tm_hour >= 4) && 
                        (et_now.tm_hour < 9 || (et_now.tm_hour == 9 && et_now.tm_min < 30));
    
    // Post-market: 4:00 PM - 8:00 PM ET
    bool in_postmarket = (et_now.tm_hour >= 16) && (et_now.tm_hour < 20);
    
    bool extended = is_weekday && (in_premarket || in_postmarket);
    
    // Restore TZ
    if (old_tz_copy[0]) {
        setenv("TZ", old_tz_copy, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    tz_unlock();
    
    ESP_LOGI(TAG_MARKET, "Extended hours check: ET %02d:%02d wday=%d, weekday=%d, premarket=%d, postmarket=%d, extended=%d",
             et_now.tm_hour, et_now.tm_min, et_now.tm_wday, is_weekday, in_premarket, in_postmarket, extended);
    
    return extended;
}

static bool fetch_time_data_old(i2c_equipment *rtc_dev, time_data_t *data)
{
    if(!data) return false;
    if (!rtc_dev) {
        time_t now = time(NULL);
        struct tm timeinfo;
        if (localtime_r(&now, &timeinfo) == NULL) {
            return false;
        }
        data->year = timeinfo.tm_year + 1900;
        data->month = timeinfo.tm_mon + 1;
        data->day = timeinfo.tm_mday;
        data->hour = timeinfo.tm_hour;
        data->minute = timeinfo.tm_min;
        data->second = timeinfo.tm_sec;
        data->week_day = timeinfo.tm_wday;
        data->day_of_year = timeinfo.tm_yday;
        data->moon_phase = 0;
        return true;
    }
    RtcDateTime_t rtc_time = rtc_dev->get_rtcTime();
    data->year = rtc_time.year;
    data->month = rtc_time.month;
    data->day = rtc_time.day;
    data->hour = rtc_time.hour;
    data->minute = rtc_time.minute;
    data->second = rtc_time.second;
    data->week_day = rtc_time.week;
    data->day_of_year = 0;
    data->moon_phase = 0;
    ESP_LOGI(TAG_STOCK, "Fetched time from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             data->year, data->month, data->day, data->hour, data->minute, data->second);
    return true;
}

uint8_t Utilities::fetch_battery_level()
{
    adc_bsp adc;
    return adc.get_Batterylevel();
}

/**
 * @brief Fetch weather data from wttr.in API
 * @param data Pointer to weather_data_t structure to fill
 * @return true if successful
 */
// Removed legacy fetch_weather_data_old()


bool Utilities::fetch_weather_data(weather_data_t *data)
{
    if (!data) return false;

    allocate_http_buffer();
    if (!http_response_buffer) {
        data->valid = false;
        return false;
    }
    http_response_len = 0;
    memset(http_response_buffer, 0, HTTP_BUFFER_SIZE);

    // Open‑Meteo hourly for today, only needed vars; use Settings
    weather_settings_t wst{};
    strncpy(wst.latitude, Settings::instance().weather_latitude(), sizeof(wst.latitude)-1);
    strncpy(wst.longitude, Settings::instance().weather_longitude(), sizeof(wst.longitude)-1);
        build_open_meteo_timezone(wst.timezone, sizeof(wst.timezone));

    char url[384];
    snprintf(url, sizeof(url),
      Utilities::weather24hours_url, wst.latitude, wst.longitude, wst.timezone );
        ESP_LOGI(TAG_WEATHER, "Hourly weather URL: %s", url);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event_handler;     // appends into http_response_buffer (+ http_response_len)
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { data->valid = false; return false; }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WEATHER, "HTTP: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        data->valid = false; return false;
    }
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || http_response_len <= 0) {
        log_weather_http_failure(url, status);
        esp_http_client_cleanup(client);
        data->valid = false; return false;
    }
    http_response_buffer[http_response_len] = '\0';
    esp_http_client_cleanup(client);

    cJSON *root = cJSON_Parse((const char*)http_response_buffer);
    if (!root) { data->valid = false; return false; }

    // Set location (simple)
    // Location name should be set via settings or API, not hardcoded
    strncpy(data->location, Settings::instance().weather_city(), sizeof(data->location)-1);
    data->location[sizeof(data->location)-1] = '\0';

    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    cJSON *timeArr = hourly ? cJSON_GetObjectItemCaseSensitive(hourly, "time") : NULL;
    cJSON *tempArr = hourly ? cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m") : NULL;
    cJSON *rhArr   = hourly ? cJSON_GetObjectItemCaseSensitive(hourly, "relative_humidity_2m") : NULL;
    cJSON *precArr = hourly ? cJSON_GetObjectItemCaseSensitive(hourly, "precipitation") : NULL;

    if (!cJSON_IsArray(timeArr) || !cJSON_IsArray(tempArr) ||
        !cJSON_IsArray(rhArr)   || !cJSON_IsArray(precArr)) {
        cJSON_Delete(root); data->valid = false; return false;
    }

    // Find entry matching current local hour
    time_t now = time(NULL);
    struct tm lt; localtime_r(&now, &lt);
    char key[64]; // "YYYY-MM-DDTHH:00"
    snprintf(key, sizeof(key), "%04d-%02d-%02dT%02d:00",
             1900+lt.tm_year, 1+lt.tm_mon, lt.tm_mday, lt.tm_hour);

    int bestIdx = 0, n = cJSON_GetArraySize(timeArr);
    for (int i=0; i<n; ++i) {
        cJSON *t = cJSON_GetArrayItem(timeArr, i);
        if (cJSON_IsString(t) && strcmp(t->valuestring, key)==0) { bestIdx = i; break; }
    }

    // Read values
    cJSON *tC = cJSON_GetArrayItem(tempArr, bestIdx);
    cJSON *rh = cJSON_GetArrayItem(rhArr, bestIdx);
    cJSON *pr = cJSON_GetArrayItem(precArr, bestIdx);

    float fTemp = cJSON_IsNumber(tC) ? (float)tC->valuedouble : 0.0f;
    int   iHum  = cJSON_IsNumber(rh) ? (int)rh->valuedouble   : 0;
    float fPre  = cJSON_IsNumber(pr) ? (float)pr->valuedouble : 0.0f;

    // condition: crude mapping (you can refine or use weather_code)
    const char *cond = (fPre > 0.0f) ? "Rain" : (iHum > 80 ? "Cloudy" : "Clear");
    strncpy(data->condition, cond, sizeof(data->condition)-1);
    data->condition[sizeof(data->condition)-1] = '\0';

    // temperature "+xx.x°C"
    char tb[16];
    if (fTemp >= 0) snprintf(tb, sizeof(tb), "%.0f°C", roundf(fTemp));
    else            snprintf(tb, sizeof(tb), "%.0f°C", roundf(fTemp));
    strncpy(data->temperature, tb, sizeof(data->temperature)-1);
    data->temperature[sizeof(data->temperature)-1] = '\0';

    // humidity "nn%"
    char hb[8]; snprintf(hb, sizeof(hb), "%d%%", iHum);
    strncpy(data->humidity, hb, sizeof(data->humidity)-1);
    data->humidity[sizeof(data->humidity)-1] = '\0';

    // rain "x.xmm"
    char rb[12]; snprintf(rb, sizeof(rb), "%.1fmm", fPre);
    strncpy(data->rain_mm, rb, sizeof(data->rain_mm)-1);
    data->rain_mm[sizeof(data->rain_mm)-1] = '\0';

    cJSON_Delete(root);
    data->valid = true;
    return true;
}

bool Utilities::fetch_current_weather_data(weather_data_t *data)
{
    if (!data) return false;

    allocate_http_buffer();
    if (!http_response_buffer) {
        data->valid = false;
        return false;
    }
    http_response_len = 0;
    memset(http_response_buffer, 0, HTTP_BUFFER_SIZE);

    weather_settings_t wst{};
    strncpy(wst.latitude, Settings::instance().weather_latitude(), sizeof(wst.latitude)-1);
    strncpy(wst.longitude, Settings::instance().weather_longitude(), sizeof(wst.longitude)-1);
    build_open_meteo_timezone(wst.timezone, sizeof(wst.timezone));

    char url[384];
    snprintf(url, sizeof(url), Utilities::weather_current_url, wst.latitude, wst.longitude, wst.timezone);
    ESP_LOGI(TAG_WEATHER, "Current weather URL: %s", url);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event_handler;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { data->valid = false; return false; }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WEATHER, "Current weather HTTP: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        data->valid = false;
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || http_response_len <= 0) {
        log_weather_http_failure(url, status);
        esp_http_client_cleanup(client);
        data->valid = false;
        return false;
    }
    http_response_buffer[http_response_len] = '\0';
    esp_http_client_cleanup(client);

    cJSON *root = cJSON_Parse((const char*)http_response_buffer);
    if (!root) { data->valid = false; return false; }

    strncpy(data->location, Settings::instance().weather_city(), sizeof(data->location)-1);
    data->location[sizeof(data->location)-1] = '\0';

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!cJSON_IsObject(current)) {
        cJSON_Delete(root);
        data->valid = false;
        return false;
    }

    cJSON *temp_obj = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    cJSON *humidity_obj = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
    cJSON *precipitation_obj = cJSON_GetObjectItemCaseSensitive(current, "precipitation");
    cJSON *weather_code_obj = cJSON_GetObjectItemCaseSensitive(current, "weather_code");

    float fTemp = cJSON_IsNumber(temp_obj) ? (float)temp_obj->valuedouble : 0.0f;
    int iHum = cJSON_IsNumber(humidity_obj) ? (int)humidity_obj->valuedouble : 0;
    float fPre = cJSON_IsNumber(precipitation_obj) ? (float)precipitation_obj->valuedouble : 0.0f;
    int weather_code = cJSON_IsNumber(weather_code_obj) ? (int)weather_code_obj->valuedouble : -1;

    const char *cond = weather_code_to_text(weather_code);
    strncpy(data->condition, cond, sizeof(data->condition)-1);
    data->condition[sizeof(data->condition)-1] = '\0';

    char tb[16]; snprintf(tb, sizeof(tb), "%.0f°C", roundf(fTemp));
    strncpy(data->temperature, tb, sizeof(data->temperature)-1);
    data->temperature[sizeof(data->temperature)-1] = '\0';

    char hb[8]; snprintf(hb, sizeof(hb), "%d%%", iHum);
    strncpy(data->humidity, hb, sizeof(data->humidity)-1);
    data->humidity[sizeof(data->humidity)-1] = '\0';

    char rb[12]; snprintf(rb, sizeof(rb), "%.1fmm", fPre);
    strncpy(data->rain_mm, rb, sizeof(data->rain_mm)-1);
    data->rain_mm[sizeof(data->rain_mm)-1] = '\0';

    cJSON_Delete(root);
    data->valid = true;
    return true;
}
// --- Power/network/time helpers moved from user_app ---------------------------------

bool Utilities::is_on_battery_power()
{
    static bool last_on_batt = true;
    const float usb_thresh_high = 4.10f;
    const float batt_thresh_low = 3.95f;
    adc_bsp adc;
    float v = adc.get_vbatVoltage();
    if (last_on_batt) {
        if (v >= usb_thresh_high) last_on_batt = false;
    } else {
        if (v <= batt_thresh_low) last_on_batt = true;
    }
    return last_on_batt;
}

// Removed unused wifi_is_connected() and ensure_wifi_connected()

/**
 * @brief Fetch next market open time from Finnhub API
 * 
 * ⚠️ SHOULD ONLY BE CALLED BY FETCH_TASK - Makes HTTP requests
 * 
 * This function:
 * 1. Checks cache (1-hour validity)
 * 2. Verifies WiFi connectivity
 * 3. Makes HTTP request to Finnhub holiday API
 * 4. Calculates next regular market open time (9:30 AM ET)
 * 5. Caches result in DataCache and NVS
 * 
 * Display/UI code should use seconds_until_market_open() instead.
 * 
 * @return Next market open time (epoch), or cached value if fetch fails, or 0 on error
 */
time_t Utilities::fetch_next_market_open_time()
{
    // Prevent concurrent TZ manipulation while computing ET windows
    tz_lock();
    time_t now = time(NULL);
    const uint32_t CACHE_DURATION = 3600;  // Cache for 1 hour
    
    // Check if DataCache has a valid cached value
    time_t cached_open = DataCache::instance().get_next_market_open_time();
    uint32_t cached_ts = DataCache::instance().get_market_open_last_fetch_ts();
    
    if (cached_open > 0 && cached_ts > 0 && ((uint32_t)now - cached_ts) < CACHE_DURATION) {
        ESP_LOGD(TAG_MARKET, "Using cached market open time from DataCache (age: %ld s)", (uint32_t)now - cached_ts);
        tz_unlock();  // CRITICAL: Always unlock before return
        return cached_open;
    }
    
    // Check WiFi connectivity before attempting network fetch
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ESP_LOGW(TAG_MARKET, "WiFi not connected - cannot fetch market open time, using cached value");
        tz_unlock();  // CRITICAL: Always unlock before return
        return cached_open > 0 ? cached_open : 0;  // Return cached value or 0
    }
    
    ESP_LOGI(TAG_MARKET, "Fetching market open time from Finnhub");
    
    const char* exchange = "US";

    // --- HTTP setup
    allocate_http_buffer();
    if (!http_response_buffer) {
        ESP_LOGE(TAG_MARKET, "Failed to allocate http_response_buffer");
        tz_unlock();  // CRITICAL: Always unlock before return
        return cached_open > 0 ? cached_open : 0;  // Return cached value if available
    }
    http_response_len = 0;
    http_overflow = false;
    http_bytes_dropped = 0;

    char url[256];
    snprintf(url, sizeof(url),
             "https://finnhub.io/api/v1/stock/market-holiday?exchange=%s", exchange);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_event_handler;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG_MARKET, "HTTP init failed for market hours");
        tz_unlock();  // CRITICAL: Always unlock before return
        return cached_open > 0 ? cached_open : 0;  // Return cached value if available
    }

    esp_http_client_set_header(cli, "X-Finnhub-Token", Settings::instance().finnhub_token());
    esp_http_client_set_header(cli, "Accept", "application/json");
    esp_http_client_set_header(cli, "User-Agent", "ESP32/1.0 (ePaper-Stock)");

    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MARKET, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        tz_unlock();  // CRITICAL: Always unlock before return
        return cached_open > 0 ? cached_open : 0;  // Return cached value if available
    }

    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || http_response_len <= 0) {
        ESP_LOGE(TAG_MARKET, "Bad HTTP status=%d len=%d", status, http_response_len);
        esp_http_client_cleanup(cli);
        tz_unlock();  // CRITICAL: Always unlock before return
        return cached_open > 0 ? cached_open : 0;  // Return cached value if available
    }
    http_response_buffer[http_response_len] = '\0';
    esp_http_client_cleanup(cli);

    // Parse the holiday data
    cJSON* root = cJSON_Parse(http_response_buffer);
    if (!root) {
        ESP_LOGE(TAG_MARKET, "JSON parse failed for market holidays");
        tz_unlock();  // CRITICAL: Always unlock before return
        return cached_open > 0 ? cached_open : 0;  // Return cached value if available
    }

    // Build set of holiday dates (format: YYYY-MM-DD)
    char holidays[50][16] = {};  // Support up to 50 holidays
    int holiday_count = 0;

    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsArray(data)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, data) {
            cJSON* atDate = cJSON_GetObjectItemCaseSensitive(item, "atDate");
            if (cJSON_IsString(atDate) && atDate->valuestring && holiday_count < 50) {
                strncpy(holidays[holiday_count], atDate->valuestring, sizeof(holidays[0]) - 1);
                holidays[holiday_count][sizeof(holidays[0]) - 1] = '\0';
                holiday_count++;
            }
        }
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG_MARKET, "Loaded %d holidays", holiday_count);

    // Calculate next market open time in US Eastern Time (handles DST)
    // Save current TZ and switch to US Eastern for calculations
    const char* old_tz = getenv("TZ");
    char old_tz_copy[64];
    old_tz_copy[0] = '\0';
    if (old_tz != NULL) {
        strncpy(old_tz_copy, old_tz, sizeof(old_tz_copy) - 1);
        old_tz_copy[sizeof(old_tz_copy) - 1] = '\0';
    }

    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();

    time_t now_et = time(NULL);
    struct tm tm_now;
    localtime_r(&now_et, &tm_now);  // now in ET

    // Helper to check if date is a holiday
    auto is_holiday = [&](int year, int month, int day) -> bool {
        char date_str[32];
        snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", year, month, day);
        for (int i = 0; i < holiday_count; i++) {
            if (strcmp(holidays[i], date_str) == 0) {
                return true;
            }
        }
        return false;
    };

    // Market opens at 9:30 AM ET
    struct tm next_open = tm_now;
    next_open.tm_hour = 9;
    next_open.tm_min = 30;
    next_open.tm_sec = 0;

    // If today's open time has passed, start from tomorrow
    time_t candidate = mktime(&next_open);
    if (candidate <= now_et) {
        next_open.tm_mday++;
        candidate = mktime(&next_open);
        localtime_r(&candidate, &next_open);  // Normalize
    }

    // Find next valid trading day (skip weekends and holidays)
    const int MAX_DAYS_SEARCH = 14;  // Search up to 2 weeks ahead
    time_t result = 0;
    for (int i = 0; i < MAX_DAYS_SEARCH; i++) {
        int wday = next_open.tm_wday;
        int year = next_open.tm_year + 1900;
        int month = next_open.tm_mon + 1;
        int day = next_open.tm_mday;

        // Check if it's a weekend
        if (wday != 0 && wday != 6) {
            // Check if it's a holiday
            if (!is_holiday(year, month, day)) {
                // Found next valid trading day
                time_t open_time = mktime(&next_open); // interprets struct as ET
                ESP_LOGI(TAG_MARKET, "Next market open (ET): %04d-%02d-%02d 09:30 (in %d seconds)",
                         year, month, day, (int)difftime(open_time, now_et));
                result = open_time;
                break;
            }
        }

        // Move to next day
        next_open.tm_mday++;
        candidate = mktime(&next_open);
        localtime_r(&candidate, &next_open);  // Normalize
    }

    // Restore original TZ BEFORE returning
    if (old_tz_copy[0] != '\0') {
        setenv("TZ", old_tz_copy, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    tz_unlock();
    
    if (result > 0) {
        // Cache the result in both static and DataCache
        // BUG FIX: Use current UTC time (not now_et) for the fetch timestamp
        // since all cache age checks use time(NULL) which is always UTC
        time_t now_utc = time(NULL);
        DataCache::instance().set_next_market_open_time(result);
        DataCache::instance().set_market_open_last_fetch_ts((uint32_t)now_utc);
        // Persist to NVS
        DataCache::instance().save_to_nvs();
        return result;
    }
    
    ESP_LOGW(TAG_MARKET, "Could not find next market open within %d days", MAX_DAYS_SEARCH);
    return cached_open > 0 ? cached_open : 0;  // Return cached value if available, or 0
}

/**
 * @brief Calculate seconds until next market open
 * 
 * ✅ SAFE FOR DISPLAY/UI CODE - Reads from cache only, no network calls
 * 
 * Reads the cached next market open time populated by fetch_task.
 * 
 * @return Seconds until market open, 0 if already open, -1 if no cached data
 */
int Utilities::seconds_until_market_open()
{
    // Read from cache only - fetch_task is responsible for populating this
    time_t next_open = DataCache::instance().get_next_market_open_time();
    if (next_open <= 0) {
        ESP_LOGD(TAG_MARKET, "No cached market open time available");
        return -1;  // Error indicator
    }

    time_t now = time(NULL);
    int diff = (int)difftime(next_open, now);
    return diff < 0 ? 0 : diff;
}

void Utilities::format_countdown_to_open(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    
    int secs = seconds_until_market_open();
    
    if (secs < 0) {
        // Error fetching data
        snprintf(out, out_sz, "Opens: --:--");
        return;
    }
    
    if (secs == 0) {
        snprintf(out, out_sz, "Opens: now");
        return;
    }
    
    // Case 1: Less than 1 hour - show HH:MM:SS
    if (secs < 3600) {
        int hours = secs / 3600;
        int mins = (secs % 3600) / 60;
        int sec = secs % 60;
        snprintf(out, out_sz, "Opens: %02d:%02d:%02d", hours, mins, sec);
    }
    // Case 2: Less than 24 hours - show H:MM
    else if (secs < 86400) {
        int hours = secs / 3600;
        int mins = (secs % 3600) / 60;
        snprintf(out, out_sz, "Opens: %d:%02d", hours, mins);
    }
    // Case 3: 1 day or more - show "X days"
    else {
        int days = secs / 86400;
        if (days == 1) {
            snprintf(out, out_sz, "Opens in 1 day");
        } else {
            snprintf(out, out_sz, "Opens in %d days", days);
        }
    }
}

/**
 * @brief Fetch next market close time from Finnhub API
 * 
 * ⚠️ SHOULD ONLY BE CALLED BY FETCH_TASK - Makes HTTP requests
 * 
 * This function:
 * 1. Verifies WiFi connectivity
 * 2. Makes HTTP request to Finnhub holiday API
 * 3. Calculates next regular market close time (4:00 PM ET)
 * 4. Does NOT cache internally - caller should save to DataCache
 * 
 * Display/UI code should use seconds_until_market_close() instead.
 * 
 * @return Next market close time (epoch), or 0 on error
 */
time_t Utilities::fetch_next_market_close_time()
{
    // Prevent concurrent TZ manipulation while computing ET windows
    tz_lock();
    const char* exchange = "US";
    
    // Check WiFi connectivity before attempting network fetch
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ESP_LOGW(TAG_MARKET, "WiFi not connected - cannot fetch market close time");
        tz_unlock();  // CRITICAL: Always unlock before return
        return 0;
    }

    // Reuse same holiday lookup logic as open time
    allocate_http_buffer();
    if (!http_response_buffer) {
        ESP_LOGE(TAG_MARKET, "Failed to allocate http_response_buffer");
        tz_unlock();  // CRITICAL: Always unlock before return
        return 0;
    }
    http_response_len = 0;
    http_overflow = false;
    http_bytes_dropped = 0;

    char url[256];
    snprintf(url, sizeof(url),
             "https://finnhub.io/api/v1/stock/market-holiday?exchange=%s", exchange);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_event_handler;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG_MARKET, "HTTP init failed for market hours");
        tz_unlock();  // CRITICAL: Always unlock before return
        return 0;
    }

    esp_http_client_set_header(cli, "X-Finnhub-Token", Settings::instance().finnhub_token());
    esp_http_client_set_header(cli, "Accept", "application/json");
    esp_http_client_set_header(cli, "User-Agent", "ESP32/1.0 (ePaper-Stock)");

    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MARKET, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        tz_unlock();  // CRITICAL: Always unlock before return
        return 0;
    }

    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || http_response_len <= 0) {
        ESP_LOGE(TAG_MARKET, "Bad HTTP status=%d len=%d", status, http_response_len);
        esp_http_client_cleanup(cli);
        tz_unlock();  // CRITICAL: Always unlock before return
        return 0;
    }
    http_response_buffer[http_response_len] = '\0';
    esp_http_client_cleanup(cli);

    cJSON* root = cJSON_Parse(http_response_buffer);
    if (!root) {
        ESP_LOGE(TAG_MARKET, "JSON parse failed for market holidays");
        tz_unlock();  // CRITICAL: Always unlock before return
        return 0;
    }

    char holidays[50][16] = {};
    int holiday_count = 0;

    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsArray(data)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, data) {
            cJSON* atDate = cJSON_GetObjectItemCaseSensitive(item, "atDate");
            if (cJSON_IsString(atDate) && atDate->valuestring && holiday_count < 50) {
                strncpy(holidays[holiday_count], atDate->valuestring, sizeof(holidays[0]) - 1);
                holidays[holiday_count][sizeof(holidays[0]) - 1] = '\0';
                holiday_count++;
            }
        }
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG_MARKET, "Loaded %d holidays for close time", holiday_count);

    const char* old_tz = getenv("TZ");
    char old_tz_copy[64];
    old_tz_copy[0] = '\0';
    if (old_tz) {
        strncpy(old_tz_copy, old_tz, sizeof(old_tz_copy) - 1);
        old_tz_copy[sizeof(old_tz_copy) - 1] = '\0';
    }

    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    auto is_holiday = [&](int year, int month, int day) -> bool {
        char date_str[32];
        snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", year, month, day);
        for (int i = 0; i < holiday_count; i++) {
            if (strcmp(holidays[i], date_str) == 0) {
                return true;
            }
        }
        return false;
    };

    // Market closes at 16:00 ET
    struct tm next_close = tm_now;
    next_close.tm_hour = 16;
    next_close.tm_min = 0;
    next_close.tm_sec = 0;

    time_t candidate = mktime(&next_close);
    // If current time is past today's close, start from tomorrow
    if (candidate <= now) {
        next_close.tm_mday++;
        candidate = mktime(&next_close);
        localtime_r(&candidate, &next_close);
    }

    const int MAX_DAYS_SEARCH = 14;
    for (int i = 0; i < MAX_DAYS_SEARCH; i++) {
        int wday = next_close.tm_wday;
        int year = next_close.tm_year + 1900;
        int month = next_close.tm_mon + 1;
        int day = next_close.tm_mday;

        if (wday != 0 && wday != 6) {
            if (!is_holiday(year, month, day)) {
                time_t close_time = mktime(&next_close);
                ESP_LOGI(TAG_MARKET, "Next market close (ET): %04d-%02d-%02d 16:00 (in %d seconds)",
                         year, month, day, (int)difftime(close_time, now));

                if (old_tz_copy[0] != '\0') {
                    setenv("TZ", old_tz_copy, 1);
                } else {
                    unsetenv("TZ");
                }
                tzset();
                tz_unlock();
                return close_time;
            }
        }

        next_close.tm_mday++;
        candidate = mktime(&next_close);
        localtime_r(&candidate, &next_close);
    }

    ESP_LOGW(TAG_MARKET, "Could not find next market close within %d days", MAX_DAYS_SEARCH);

    if (old_tz_copy[0] != '\0') {
        setenv("TZ", old_tz_copy, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    tz_unlock();
    return 0;
}

/**
 * @brief Calculate seconds until next market close
 * 
 * ✅ SAFE FOR DISPLAY/UI CODE - Reads from cache only, no network calls
 * 
 * Reads the cached next market close time populated by fetch_task.
 * 
 * @return Seconds until market close, 0 if already closed, -1 if no cached data
 */
int Utilities::seconds_until_market_close()
{
    // Use cached close time if available
    time_t next_close = DataCache::instance().get_next_market_close_time();
    if (next_close == 0) {
        ESP_LOGW(TAG_MARKET, "No cached market close time available");
        return -1;  // Error indicator
    }
    
    time_t now = time(NULL);
    int diff = (int)difftime(next_close, now);
    return diff < 0 ? 0 : diff;
}

void Utilities::format_countdown_to_close(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    int secs = seconds_until_market_close();
    if (secs < 0) {
        snprintf(out, out_sz, "Closing: --:--");
        return;
    }
    if (secs == 0) {
        snprintf(out, out_sz, "Closed: now");
        return;
    }
    if (secs < 3600) {
        int hours = secs / 3600;
        int mins = (secs % 3600) / 60;
        int sec = secs % 60;
        snprintf(out, out_sz, "Closing: %02d:%02d:%02d", hours, mins, sec);
    } else if (secs < 86400) {
        int hours = secs / 3600;
        int mins = (secs % 3600) / 60;
        snprintf(out, out_sz, "Closing: %d:%02d", hours, mins);
    } else {
        int days = secs / 86400;
        if (days == 1) {
            snprintf(out, out_sz, "Closed in 1 day");
        } else {
            snprintf(out, out_sz, "Closed in %d days", days);
        }
    }
}

// Removed delete_all_tasks_but_current()

// ============================================================================
// TIME CONVERSION - SINGLE SOURCE OF TRUTH
// ============================================================================
// ARCHITECTURE:
// - The RTC chip stores time in UTC
// - ESP32 system time (time_t from time()) is always UTC epoch
// - The TZ environment variable (set via Settings) defines the user's timezone
// - localtime_r() applies TZ to convert UTC epoch to local time
// - mktime() interprets a tm struct according to current TZ
//
// RULES:
// 1. All time_t variables MUST be named with "_utc" suffix (e.g., system_time_utc)
// 2. All tm structs MUST be named with "_utc" or "local_" prefix (e.g., local_tm, utc_tm)
// 3. RTC reads/writes are always UTC - variables named "rtc_time_utc"
// 4. Use ONLY these centralized functions for timezone conversion
// 5. Never call setenv("TZ", ...) except in these functions
//
// CENTRALIZED FUNCTIONS:
// - utc_epoch_to_local_tm(): Converts UTC epoch to local tm (for display)
// - utc_tm_to_epoch(): Converts UTC tm to epoch (for RTC reads)
// ============================================================================

bool Utilities::utc_epoch_to_local_tm(time_t utc_epoch, struct tm *local_tm_out)
{
    if (!local_tm_out) return false;
    
    // Use localtime_r which applies the TZ environment variable timezone offset
    if (localtime_r(&utc_epoch, local_tm_out) == nullptr) {
        ESP_LOGE("time", "localtime_r failed for epoch=%ld", (long)utc_epoch);
        return false;
    }
    
    return true;
}

time_t Utilities::utc_tm_to_epoch(const struct tm *utc_tm)
{
    if (!utc_tm) return 0;
    
    // To convert UTC tm to epoch, we must temporarily set TZ=UTC0
    // because mktime() interprets the tm according to the current timezone
    const char *user_tz = Settings::instance().timezone();
    setenv("TZ", "UTC0", 1);
    tzset();
    
    // Make a copy since mktime modifies the struct
    struct tm tm_copy = *utc_tm;
    time_t epoch_utc = mktime(&tm_copy);
    
    // Restore user's timezone
    setenv("TZ", user_tz, 1);
    tzset();
    
    if (epoch_utc == -1) {
        ESP_LOGE("time", "mktime failed for UTC time");
        return 0;
    }
    
    return epoch_utc;
}

bool Utilities::fetch_time_data(i2c_equipment *rtc_dev, time_data_t *data)
{
    if (!data) return false;

    // Get current system time (stored internally as UTC epoch)
    time_t system_time_utc = time(NULL);
    struct tm local_tm;
    const char *tz_env = getenv("TZ");
    struct tm utc_tm;
    gmtime_r(&system_time_utc, &utc_tm);
    
    // Use centralized UTC-to-local conversion (SINGLE SOURCE OF TRUTH)
    if (!utc_epoch_to_local_tm(system_time_utc, &local_tm)) {
        // Fallback: if conversion fails and RTC is available, read RTC directly
        if (!rtc_dev) {
            ESP_LOGE("time", "UTC-to-local conversion failed and no RTC available");
            return false;
        }
        
        RtcDateTime_t rtc_time_utc = rtc_dev->get_rtcTime();  // RTC stores UTC

        struct tm rtc_utc_tm = {};
        rtc_utc_tm.tm_year = rtc_time_utc.year - 1900;
        rtc_utc_tm.tm_mon  = rtc_time_utc.month - 1;
        rtc_utc_tm.tm_mday = rtc_time_utc.day;
        rtc_utc_tm.tm_hour = rtc_time_utc.hour;
        rtc_utc_tm.tm_min  = rtc_time_utc.minute;
        rtc_utc_tm.tm_sec  = rtc_time_utc.second;
        rtc_utc_tm.tm_isdst = 0;

        // Use centralized UTC-to-epoch conversion (SINGLE SOURCE OF TRUTH)
        time_t rtc_epoch_utc = utc_tm_to_epoch(&rtc_utc_tm);
        
        // Use centralized epoch-to-local conversion (SINGLE SOURCE OF TRUTH)
        if (rtc_epoch_utc <= 0 || !utc_epoch_to_local_tm(rtc_epoch_utc, &local_tm)) {
            ESP_LOGE("time", "RTC fallback conversion failed");
            return false;
        }
        
        system_time_utc = rtc_epoch_utc;
    }

    ESP_LOGI("time", "TZ='%s' epoch_utc=%ld local=%04d-%02d-%02d %02d:%02d:%02d utc=%04d-%02d-%02d %02d:%02d:%02d",
             tz_env ? tz_env : "(null)", (long)system_time_utc,
             local_tm.tm_year+1900, local_tm.tm_mon+1, local_tm.tm_mday, local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
             utc_tm.tm_year+1900, utc_tm.tm_mon+1, utc_tm.tm_mday, utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);

    data->year      = local_tm.tm_year + 1900;
    data->month     = local_tm.tm_mon + 1;
    data->day       = local_tm.tm_mday;
    data->hour      = local_tm.tm_hour;
    data->minute    = local_tm.tm_min;
    data->second    = local_tm.tm_sec;
    data->week_day  = local_tm.tm_wday;
    data->day_of_year = local_tm.tm_yday;
    data->moon_phase  = 0; // not implemented
    return true;
}

