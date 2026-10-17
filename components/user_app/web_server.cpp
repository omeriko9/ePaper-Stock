#include "web_server.h"
#include "settings.h"
#include "user_app.h"
#include "audio_player.h"
#include "adc_bsp.h"
#include "data_cache.h"
#include "audio_bsp.h"
#include "log_capture.h"
#include "user_config.h"

#include <esp_http_server.h>
#include <esp_spiffs.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <cJSON.h>
#include <sys/stat.h>
#include <cstring>
#include <time.h>
#include "utilities.h"

#include <Arduino.h>
#include <WiFi.h>

static const char *TAG = "websrv";
static const char *DEAD_BATTERY_IMAGE_PATH = "/spiffs/dead_battery_image.bin";
static constexpr size_t DEAD_BATTERY_IMAGE_BYTES = (EPD_WIDTH * EPD_HEIGHT) / 8;

static httpd_handle_t s_server = nullptr;
static unsigned s_port_running = 0;

// Forward declare new UI handlers to avoid scope errors
static esp_err_t handle_user_ui(httpd_req_t *req);
static esp_err_t handle_admin_ui(httpd_req_t *req);

// Forward handlers
static esp_err_t handle_static(httpd_req_t *req);
static esp_err_t handle_app_js(httpd_req_t *req);
static esp_err_t handle_theme_css(httpd_req_t *req);
static esp_err_t handle_font_ttf(httpd_req_t *req);
// Removed unused Inter font endpoint
// static esp_err_t handle_font_otf(httpd_req_t* req);
// Audio management endpoints
static esp_err_t handle_get_audio_file(httpd_req_t *req);
static esp_err_t handle_get_sound_files(httpd_req_t *req);
static esp_err_t handle_post_sound_upload(httpd_req_t *req);
static esp_err_t handle_get_config(httpd_req_t *req);
static esp_err_t handle_post_config(httpd_req_t *req);
static esp_err_t handle_get_status(httpd_req_t *req);
static esp_err_t handle_post_restart(httpd_req_t *req);
static esp_err_t handle_post_wifi_wipe(httpd_req_t *req);
static esp_err_t handle_post_cache_clear(httpd_req_t *req);
static esp_err_t handle_get_wifi_info(httpd_req_t *req);
static esp_err_t handle_post_sleep_toggle(httpd_req_t *req);
static esp_err_t handle_post_refresh(httpd_req_t *req);
static esp_err_t handle_get_log(httpd_req_t *req);
static esp_err_t handle_post_force_flush(httpd_req_t *req);
static esp_err_t handle_post_clear_log(httpd_req_t *req);
static esp_err_t handle_post_test_sound(httpd_req_t *req);
static esp_err_t handle_post_test_tone(httpd_req_t *req);
static esp_err_t handle_post_dead_battery_image_upload(httpd_req_t *req);
static esp_err_t handle_get_dead_battery_image_status(httpd_req_t *req);
static esp_err_t handle_post_dead_battery_image_test(httpd_req_t *req);

static bool dead_battery_image_exists(size_t *out_size = nullptr)
{
    struct stat st;
    if (stat(DEAD_BATTERY_IMAGE_PATH, &st) != 0)
    {
        if (out_size)
            *out_size = 0;
        return false;
    }
    if (out_size)
        *out_size = static_cast<size_t>(st.st_size);
    return true;
}


static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool read_body(httpd_req_t *req, std::string &out)
{
    int total = req->content_len;
    if (total <= 0)
    {
        out.clear();
        return true;
    }
    out.resize(total);
    int off = 0;
    while (off < total)
    {
        int r = httpd_req_recv(req, &out[off], total - off);
        if (r <= 0)
            return false;
        off += r;
    }
    return true;
}

bool web_server_start()
{
    if (s_server)
        return true;
    // Ensure LWIP/esp_netif core is initialized before any sockets are created
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();
    unsigned port = Settings::instance().server_port();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = (uint16_t)port;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 28;
    cfg.stack_size = 16384;    // 16KB stack - SPIFFS operations during garbage collection need significant stack

    if (httpd_start(&s_server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start server on port %u", port);
        s_server = nullptr;
        return false;
    }
    s_port_running = port;
    ESP_LOGI(TAG, "HTTP server started on port %u", port);

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handle_static, .user_ctx = nullptr};
    httpd_uri_t root_user = {.uri = "/", .method = HTTP_GET, .handler = handle_user_ui, .user_ctx = nullptr};
    httpd_uri_t index_html = {.uri = "/index.html", .method = HTTP_GET, .handler = handle_user_ui, .user_ctx = nullptr};
    httpd_uri_t admin_ui = {.uri = "/admin.html", .method = HTTP_GET, .handler = handle_admin_ui, .user_ctx = nullptr};
    httpd_uri_t appjs = {.uri = "/app.js", .method = HTTP_GET, .handler = handle_app_js, .user_ctx = nullptr};
    httpd_uri_t theme_css = {.uri = "/theme.css", .method = HTTP_GET, .handler = handle_theme_css, .user_ctx = nullptr};
    httpd_uri_t font_ttf = {.uri = "/NotoSans-Regular.ttf", .method = HTTP_GET, .handler = handle_font_ttf, .user_ctx = nullptr};
    // Removed Inter-Regular.otf endpoint

    httpd_register_uri_handler(s_server, &root_user);
    httpd_register_uri_handler(s_server, &index_html);
    httpd_register_uri_handler(s_server, &admin_ui);
    httpd_register_uri_handler(s_server, &appjs);
    httpd_register_uri_handler(s_server, &theme_css);
    httpd_register_uri_handler(s_server, &font_ttf);

    // Audio endpoints
    httpd_uri_t audio_file = {.uri = "/audio/*", .method = HTTP_GET, .handler = handle_get_audio_file, .user_ctx = nullptr};
    httpd_register_uri_handler(s_server, &audio_file);

    httpd_uri_t get_sound_files = {.uri = "/api/sound/files", .method = HTTP_GET, .handler = handle_get_sound_files, .user_ctx = nullptr};
    httpd_register_uri_handler(s_server, &get_sound_files);

    httpd_uri_t post_sound_upload = {.uri = "/api/sound/upload", .method = HTTP_POST, .handler = handle_post_sound_upload, .user_ctx = nullptr};
    httpd_register_uri_handler(s_server, &post_sound_upload);

    httpd_uri_t get_cfg = {.uri = "/api/config", .method = HTTP_GET, .handler = handle_get_config, .user_ctx = nullptr};
    httpd_uri_t post_cfg = {.uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config, .user_ctx = nullptr};
    httpd_uri_t get_status = {.uri = "/api/status", .method = HTTP_GET, .handler = handle_get_status, .user_ctx = nullptr};
    httpd_uri_t post_restart = {.uri = "/api/restart", .method = HTTP_POST, .handler = handle_post_restart, .user_ctx = nullptr};
    httpd_uri_t post_wipe = {.uri = "/api/wifi/wipe", .method = HTTP_POST, .handler = handle_post_wifi_wipe, .user_ctx = nullptr};
    httpd_uri_t post_cache_clear = {.uri = "/api/cache/clear", .method = HTTP_POST, .handler = handle_post_cache_clear, .user_ctx = nullptr};
    httpd_uri_t get_wifi = {.uri = "/api/wifi/info", .method = HTTP_GET, .handler = handle_get_wifi_info, .user_ctx = nullptr};
    httpd_uri_t post_sleep = {.uri = "/api/sleep", .method = HTTP_POST, .handler = handle_post_sleep_toggle, .user_ctx = nullptr};
    httpd_uri_t post_refresh = {.uri = "/api/refresh", .method = HTTP_POST, .handler = handle_post_refresh, .user_ctx = nullptr};
    httpd_uri_t post_test_sound = {.uri = "/api/test_sound", .method = HTTP_POST, .handler = handle_post_test_sound, .user_ctx = nullptr};
    httpd_uri_t post_test_tone = {.uri = "/api/test_tone", .method = HTTP_POST, .handler = handle_post_test_tone, .user_ctx = nullptr};
    httpd_uri_t post_dead_battery_image_upload = {.uri = "/api/dead_battery_image/upload", .method = HTTP_POST, .handler = handle_post_dead_battery_image_upload, .user_ctx = nullptr};
    httpd_uri_t get_dead_battery_image_status = {.uri = "/api/dead_battery_image/status", .method = HTTP_GET, .handler = handle_get_dead_battery_image_status, .user_ctx = nullptr};
    httpd_uri_t post_dead_battery_image_test = {.uri = "/api/dead_battery_image/test", .method = HTTP_POST, .handler = handle_post_dead_battery_image_test, .user_ctx = nullptr};
    httpd_uri_t get_log = {.uri = "/api/log", .method = HTTP_GET, .handler = handle_get_log, .user_ctx = nullptr};
    httpd_uri_t post_force_flush = {.uri = "/api/force_flush", .method = HTTP_POST, .handler = handle_post_force_flush, .user_ctx = nullptr};
    httpd_uri_t post_clear_log = {.uri = "/api/clear_log", .method = HTTP_POST, .handler = handle_post_clear_log, .user_ctx = nullptr};

    httpd_register_uri_handler(s_server, &get_cfg);
    httpd_register_uri_handler(s_server, &post_cfg);
    httpd_register_uri_handler(s_server, &get_status);
    httpd_register_uri_handler(s_server, &post_restart);
    httpd_register_uri_handler(s_server, &post_wipe);
    httpd_register_uri_handler(s_server, &post_cache_clear);
    httpd_register_uri_handler(s_server, &get_wifi);
    httpd_register_uri_handler(s_server, &post_sleep);
    httpd_register_uri_handler(s_server, &post_refresh);
    httpd_register_uri_handler(s_server, &post_test_sound);
    httpd_register_uri_handler(s_server, &post_test_tone);
    httpd_register_uri_handler(s_server, &post_dead_battery_image_upload);
    httpd_register_uri_handler(s_server, &get_dead_battery_image_status);
    httpd_register_uri_handler(s_server, &post_dead_battery_image_test);
    httpd_register_uri_handler(s_server, &get_log);
    httpd_register_uri_handler(s_server, &post_force_flush);
    httpd_register_uri_handler(s_server, &post_clear_log);

    return true;
}

void web_server_stop()
{
    if (s_server)
    {
        ESP_LOGI(TAG, "Stopping HTTP server on port %u", s_port_running);
        httpd_stop(s_server);
        s_server = nullptr;
        s_port_running = 0;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    else
    {
        ESP_LOGI(TAG, "HTTP server already stopped");
    }
}

bool web_server_restart_if_needed(unsigned new_port)
{
    if (s_port_running == new_port && s_server)
        return true;
    web_server_stop();
    return web_server_start();
}

bool web_server_is_running()
{
    return s_server != nullptr;
}

// ========== Static file serving ==========

static bool send_file(httpd_req_t *req, const char *path, const char *ctype)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    httpd_resp_set_type(req, ctype);
    uint8_t buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK)
        {
            fclose(f);
            httpd_resp_sendstr_chunk(req, nullptr);
            return false;
        }
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, nullptr);
    return true;
}

static esp_err_t handle_user_ui(httpd_req_t *req)
{
    return send_file(req, "/spiffs/index.html", "text/html; charset=utf-8")
               ? ESP_OK
               : (httpd_resp_send_404(req), ESP_OK);
}

static esp_err_t handle_admin_ui(httpd_req_t *req)
{
    return send_file(req, "/spiffs/admin.html", "text/html; charset=utf-8")
               ? ESP_OK
               : (httpd_resp_send_404(req), ESP_OK);
}

// Maintain backward compatibility for static index fetch
static esp_err_t handle_static(httpd_req_t *req)
{
    return handle_user_ui(req);
}

static esp_err_t handle_app_js(httpd_req_t *req)
{
    if (!send_file(req, "/spiffs/app.js", "application/javascript; charset=utf-8"))
    {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

static esp_err_t handle_theme_css(httpd_req_t *req)
{
    if (!send_file(req, "/spiffs/theme.css", "text/css; charset=utf-8"))
    {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

static esp_err_t handle_font_ttf(httpd_req_t *req)
{
    if (!send_file(req, "/spiffs/NotoSans-Regular.ttf", "font/ttf"))
    {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

// ======= Audio helpers & endpoints =======

static bool ensure_audio_mounted()
{
    if (esp_spiffs_mounted("audio"))
        return true;
    esp_vfs_spiffs_conf_t audio_conf = {
        .base_path = "/audio",
        .partition_label = "audio",
        .max_files = 3,
        .format_if_mount_failed = false};
    esp_err_t r = esp_vfs_spiffs_register(&audio_conf);
    if (r != ESP_OK)
    {
        ESP_LOGW(TAG, "Audio SPIFFS mount failed: %s", esp_err_to_name(r));
        return false;
    }
    return true;
}

static bool name_is_allowed(const char *name)
{
    // Restrict to top-level files, whitelist known names used by firmware
    static const char *allowed[] = {
        "market_open.mp3", "startup.mp3", "stock_high.mp3", "stock_low.mp3",
        // Allow also existing aux sounds, though UI only shows 4
        "server_up.mp3", "server_down.mp3", "pop.mp3", "ting.mp3", "whoosh.mp3", "error.mp3"};
    if (!name || strchr(name, '/'))
        return false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
    {
        if (strcmp(name, allowed[i]) == 0)
            return true;
    }
    return false;
}

static bool name_is_allowed_spiffs(const char *name)
{
    // Allow updating static web assets only
    static const char *allowed[] = {
        "index.html", "admin.html", "app.js", "theme.css", "NotoSans-Regular.ttf"};
    if (!name || strchr(name, '/'))
        return false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
    {
        if (strcmp(name, allowed[i]) == 0)
            return true;
    }
    return false;
}

static esp_err_t handle_get_audio_file(httpd_req_t *req)
{
    if (!ensure_audio_mounted())
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "audio not mounted");
        return ESP_OK;
    }
    const char *uri = req->uri; // "/audio/<name>"
    const char *p = strstr(uri, "/audio/");
    if (!p)
    {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    p += 7; // skip "/audio/"
    if (!*p)
    {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    // deny traversal
    if (strchr(p, '/'))
    {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    char path[128];
    snprintf(path, sizeof(path), "/audio/%s", p);
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "audio/mpeg");
    uint8_t buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK)
        {
            fclose(f);
            httpd_resp_sendstr_chunk(req, nullptr);
            return ESP_OK;
        }
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

static esp_err_t handle_get_sound_files(httpd_req_t *req)
{
    if (!ensure_audio_mounted())
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "audio not mounted");
        return ESP_OK;
    }
    const char *names[] = {"market_open.mp3", "startup.mp3", "stock_high.mp3", "stock_low.mp3"};
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    {
        char p[64];
        snprintf(p, sizeof(p), "/audio/%s", names[i]);
        struct stat st;
        memset(&st, 0, sizeof(st));
        bool exists = (stat(p, &st) == 0);
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "name", names[i]);
        cJSON_AddBoolToObject(it, "exists", exists);
        cJSON_AddNumberToObject(it, "size", exists ? (double)st.st_size : 0.0);
        cJSON_AddItemToArray(arr, it);
    }
    cJSON_AddItemToObject(o, "files", arr);
    // Include SPIFFS usage stats for accurate free-space display
    size_t total_bytes = 0, used_bytes = 0;
    if (esp_spiffs_info("audio", &total_bytes, &used_bytes) == ESP_OK)
    {
        size_t free_bytes = (total_bytes > used_bytes) ? (total_bytes - used_bytes) : 0;
        cJSON_AddNumberToObject(o, "total_bytes", (double)total_bytes);
        cJSON_AddNumberToObject(o, "used_bytes", (double)used_bytes);
        cJSON_AddNumberToObject(o, "free_bytes", (double)free_bytes);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    send_json(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t handle_post_sound_upload(httpd_req_t *req)
{
    // Parse ?name= and optional ?dir=
    int qlen = httpd_req_get_url_query_len(req);
    std::string q;
    q.resize(qlen + 1);
    if (qlen <= 0 || httpd_req_get_url_query_str(req, q.data(), q.size()) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
        return ESP_OK;
    }
    char name[64] = {0};
    char dir[16] = {0};
    (void)httpd_query_key_value(q.c_str(), "dir", dir, sizeof(dir));
    if (httpd_query_key_value(q.c_str(), "name", name, sizeof(name)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
        return ESP_OK;
    }

    bool to_audio = !(dir[0]) || strcmp(dir, "audio") == 0;
    if (to_audio)
    {
        if (!ensure_audio_mounted())
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "audio not mounted");
            return ESP_OK;
        }
        if (!name_is_allowed(name))
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
            return ESP_OK;
        }
    }
    else
    {
        // Upload to /spiffs
        if (!name_is_allowed_spiffs(name))
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
            return ESP_OK;
        }
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", to_audio ? "/audio" : "/spiffs", name);
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }
    int remain = req->content_len;
    char buf[1024];
    while (remain > 0)
    {
        int r = httpd_req_recv(req, buf, remain > (int)sizeof(buf) ? (int)sizeof(buf) : remain);
        if (r <= 0)
        {
            fclose(f);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        fwrite(buf, 1, (size_t)r, f);
        remain -= r;
    }
    long sz = ftell(f);
    fclose(f);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "dir", to_audio ? "audio" : "spiffs");
    cJSON_AddNumberToObject(o, "size", (double)sz);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    send_json(req, s);
    free(s);
    return ESP_OK;
}

// ========== APIs ==========

static float round2f(float x)
{
    // Round to 2 decimal places without printing long binary tails
    // Handles negative values correctly
    return floorf(x * 100.0f + (x >= 0 ? 0.5f : -0.5f)) / 100.0f;
}

static cJSON *settings_to_json(const SettingsData &d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "server_port", d.server_port);
    cJSON_AddNumberToObject(o, "sleep_interval_ms", (double)d.sleep_interval_ms);
    cJSON_AddNumberToObject(o, "awake_after_update_ms", (double)d.awake_after_update_ms);
    cJSON_AddNumberToObject(o, "market_closed_sleep_ms", (double)d.market_closed_sleep_ms);
    cJSON_AddStringToObject(o, "stock_symbol", d.stock_symbol); // Deprecated, keep for compat
    cJSON_AddStringToObject(o, "weather_city", d.weather_city);
    cJSON_AddStringToObject(o, "timezone", d.timezone);
    // Weather (Open‑Meteo) & intervals
    cJSON_AddStringToObject(o, "weather_latitude", d.weather_latitude);
    cJSON_AddStringToObject(o, "weather_longitude", d.weather_longitude);
    cJSON_AddStringToObject(o, "weather_tz", d.weather_tz);
    cJSON_AddNumberToObject(o, "stock_fetch_interval_ms", (double)d.stock_fetch_interval_ms);
    //cJSON_AddNumberToObject(o, "weather24_fetch_interval_ms", (double)d.weather24_fetch_interval_ms);
    //cJSON_AddNumberToObject(o, "weather_current_fetch_interval_ms", (double)d.weather_current_fetch_interval_ms);
    // New interval settings
    cJSON_AddNumberToObject(o, "device_sleep_interval_ms", (double)d.device_sleep_interval_ms);
    //cJSON_AddNumberToObject(o, "stock_fetch_interval_closed_ms", (double)d.stock_fetch_interval_closed_ms);
    //cJSON_AddNumberToObject(o, "stock_fetch_interval_preopen_ms", (double)d.stock_fetch_interval_preopen_ms);
    //cJSON_AddNumberToObject(o, "market_check_interval_ms", (double)d.market_check_interval_ms);
    cJSON_AddNumberToObject(o, "stock_carousel_interval_ms", (double)d.stock_carousel_interval_ms);
    cJSON_AddBoolToObject(o, "device_sleep_enabled", d.device_sleep_enabled);
    cJSON_AddBoolToObject(o, "sound_enabled", d.sound_enabled);
    cJSON_AddBoolToObject(o, "mock_mode_enabled", d.mock_mode_enabled);
    cJSON_AddBoolToObject(o, "log_capture_enabled", d.log_capture_enabled);
    cJSON_AddBoolToObject(o, "dead_battery_image_enabled", d.dead_battery_image_enabled);
    // API tokens
    cJSON_AddStringToObject(o, "finnhub_token", d.finnhub_token);
    // Round floats to 2 decimals to avoid long binary tails in JSON
    cJSON_AddNumberToObject(o, "stock_max_sound", (double)round2f(d.stock_max_sound)); // Deprecated
    cJSON_AddNumberToObject(o, "stock_min_sound", (double)round2f(d.stock_min_sound)); // Deprecated
    cJSON_AddNumberToObject(o, "audio_volume", (double)d.audio_volume);

    // Add stock array
    cJSON *stocks_array = cJSON_CreateArray();
    for (uint8_t i = 0; i < d.stock_count && i < 10; i++)
    {
        cJSON *stock_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(stock_obj, "symbol", d.stocks[i].symbol);
        cJSON_AddNumberToObject(stock_obj, "min_sound", (double)round2f(d.stocks[i].min_sound));
        cJSON_AddNumberToObject(stock_obj, "max_sound", (double)round2f(d.stocks[i].max_sound));
        cJSON_AddItemToArray(stocks_array, stock_obj);
    }
    cJSON_AddItemToObject(o, "stocks", stocks_array);
    cJSON_AddNumberToObject(o, "stock_count", d.stock_count);
    // Include persisted log level (default INFO=3)
    {
        nvs_handle_t nvs;
        uint8_t lvl = 3;
        if (nvs_open("config", NVS_READONLY, &nvs) == ESP_OK)
        {
            uint8_t v = 3;
            if (nvs_get_u8(nvs, "log_lvl", &v) == ESP_OK)
                lvl = v;
            nvs_close(nvs);
        }
        cJSON_AddNumberToObject(o, "log_level", (double)lvl);
    }

    return o;
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    SettingsData d = Settings::instance().get();
    cJSON *o = settings_to_json(d);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    send_json(req, str);
    free(str);
    return ESP_OK;
}

static bool parse_bool(cJSON *o, const char *key, bool def)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    if (!v)
        return def;
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0.0;
    if (cJSON_IsString(v))
        return strcmp(v->valuestring, "true") == 0 || strcmp(v->valuestring, "1") == 0;
    return def;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    std::string body;
    if (!read_body(req, body))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read body failed");
        return ESP_OK;
    }
    cJSON *in = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!in)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_OK;
    }

    SettingsData cur = Settings::instance().get();
    SettingsData next = cur;

    cJSON *v;
    if ((v = cJSON_GetObjectItem(in, "server_port")) && cJSON_IsNumber(v))
        next.server_port = (uint16_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "sleep_interval_ms")) && cJSON_IsNumber(v))
        next.sleep_interval_ms = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "awake_after_update_ms")) && cJSON_IsNumber(v))
        next.awake_after_update_ms = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "market_closed_sleep_ms")) && cJSON_IsNumber(v))
        next.market_closed_sleep_ms = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "stock_symbol")) && cJSON_IsString(v))
        strncpy(next.stock_symbol, v->valuestring, sizeof(next.stock_symbol));
    if ((v = cJSON_GetObjectItem(in, "weather_city")) && cJSON_IsString(v))
        strncpy(next.weather_city, v->valuestring, sizeof(next.weather_city));
    if ((v = cJSON_GetObjectItem(in, "timezone")) && cJSON_IsString(v))
        strncpy(next.timezone, v->valuestring, sizeof(next.timezone));
    if ((v = cJSON_GetObjectItem(in, "weather_latitude")) && cJSON_IsString(v))
        strncpy(next.weather_latitude, v->valuestring, sizeof(next.weather_latitude));
    if ((v = cJSON_GetObjectItem(in, "weather_longitude")) && cJSON_IsString(v))
        strncpy(next.weather_longitude, v->valuestring, sizeof(next.weather_longitude));
    if ((v = cJSON_GetObjectItem(in, "weather_tz")) && cJSON_IsString(v))
        strncpy(next.weather_tz, v->valuestring, sizeof(next.weather_tz));
    if ((v = cJSON_GetObjectItem(in, "stock_fetch_interval_ms")) && cJSON_IsNumber(v))
        next.stock_fetch_interval_ms = (uint32_t)v->valuedouble;
    // if ((v = cJSON_GetObjectItem(in, "weather24_fetch_interval_ms")) && cJSON_IsNumber(v))
    //     next.weather24_fetch_interval_ms = (uint32_t)v->valuedouble;
    // if ((v = cJSON_GetObjectItem(in, "weather_current_fetch_interval_ms")) && cJSON_IsNumber(v))
    //     next.weather_current_fetch_interval_ms = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "device_sleep_interval_ms")) && cJSON_IsNumber(v))
        next.device_sleep_interval_ms = (uint32_t)v->valuedouble;
    // if ((v = cJSON_GetObjectItem(in, "stock_fetch_interval_closed_ms")) && cJSON_IsNumber(v))
    //     next.stock_fetch_interval_closed_ms = (uint32_t)v->valuedouble;
    // if ((v = cJSON_GetObjectItem(in, "stock_fetch_interval_preopen_ms")) && cJSON_IsNumber(v))
    //     next.stock_fetch_interval_preopen_ms = (uint32_t)v->valuedouble;
    // if ((v = cJSON_GetObjectItem(in, "market_check_interval_ms")) && cJSON_IsNumber(v))
    //     next.market_check_interval_ms = (uint32_t)v->valuedouble;
    next.device_sleep_enabled = parse_bool(in, "device_sleep_enabled", cur.device_sleep_enabled);
    next.sound_enabled = parse_bool(in, "sound_enabled", cur.sound_enabled);
    next.mock_mode_enabled = parse_bool(in, "mock_mode_enabled", cur.mock_mode_enabled);
    next.log_capture_enabled = parse_bool(in, "log_capture_enabled", cur.log_capture_enabled);
    next.dead_battery_image_enabled = parse_bool(in, "dead_battery_image_enabled", cur.dead_battery_image_enabled);
    if ((v = cJSON_GetObjectItem(in, "stock_max_sound")) && cJSON_IsNumber(v))
        next.stock_max_sound = (float)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "stock_min_sound")) && cJSON_IsNumber(v))
        next.stock_min_sound = (float)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "audio_volume")) && cJSON_IsNumber(v))
    {
        next.audio_volume = (uint8_t)v->valuedouble;
        if (next.audio_volume > 100)
            next.audio_volume = 100;
    }
    // Optional Finnhub token
    if ((v = cJSON_GetObjectItem(in, "finnhub_token")) && cJSON_IsString(v))
    {
        strncpy(next.finnhub_token, v->valuestring, sizeof(next.finnhub_token));
    }
    // Optional log_level (0..5)
    int log_level_num = -1;
    if ((v = cJSON_GetObjectItem(in, "log_level")) && cJSON_IsNumber(v))
    {
        log_level_num = (int)v->valuedouble;
        if (log_level_num < (int)ESP_LOG_NONE)
            log_level_num = (int)ESP_LOG_NONE;
        if (log_level_num > (int)ESP_LOG_VERBOSE)
            log_level_num = (int)ESP_LOG_VERBOSE;
        // Persist the selected log level
        nvs_handle_t n;
        if (nvs_open("config", NVS_READWRITE, &n) == ESP_OK)
        {
            nvs_set_u8(n, "log_lvl", (uint8_t)log_level_num);
            nvs_commit(n);
            nvs_close(n);
        }
        // Apply immediately to all components
        esp_log_level_set("*", (esp_log_level_t)log_level_num);
        ESP_LOGI(TAG, "Applied new log level: %d", log_level_num);
    }
    if ((v = cJSON_GetObjectItem(in, "stock_carousel_interval_ms")) && cJSON_IsNumber(v))
        next.stock_carousel_interval_ms = (uint32_t)v->valuedouble;

    // Parse stock array
    if ((v = cJSON_GetObjectItem(in, "stocks")) && cJSON_IsArray(v))
    {
        int arr_size = cJSON_GetArraySize(v);
        if (arr_size > 10)
            arr_size = 10;
        next.stock_count = (uint8_t)arr_size;
        memset(next.stocks, 0, sizeof(next.stocks));
        for (int i = 0; i < arr_size; i++)
        {
            cJSON *stock_obj = cJSON_GetArrayItem(v, i);
            if (stock_obj && cJSON_IsObject(stock_obj))
            {
                cJSON *sym = cJSON_GetObjectItem(stock_obj, "symbol");
                if (sym && cJSON_IsString(sym))
                {
                    strncpy(next.stocks[i].symbol, sym->valuestring, sizeof(next.stocks[i].symbol));
                }
                cJSON *min_snd = cJSON_GetObjectItem(stock_obj, "min_sound");
                if (min_snd && cJSON_IsNumber(min_snd))
                {
                    next.stocks[i].min_sound = (float)min_snd->valuedouble;
                }
                cJSON *max_snd = cJSON_GetObjectItem(stock_obj, "max_sound");
                if (max_snd && cJSON_IsNumber(max_snd))
                {
                    next.stocks[i].max_sound = (float)max_snd->valuedouble;
                }
                next.stocks[i].symbol[sizeof(next.stocks[i].symbol) - 1] = '\0';
            }
        }
    }

    // Ensure null-termination
    next.stock_symbol[sizeof(next.stock_symbol) - 1] = '\0';
    next.weather_city[sizeof(next.weather_city) - 1] = '\0';
    next.timezone[sizeof(next.timezone) - 1] = '\0';
    next.weather_latitude[sizeof(next.weather_latitude) - 1] = '\0';
    next.weather_longitude[sizeof(next.weather_longitude) - 1] = '\0';
    next.weather_tz[sizeof(next.weather_tz) - 1] = '\0';
    next.finnhub_token[sizeof(next.finnhub_token) - 1] = '\0';

    bool weather_location_changed = strcmp(cur.weather_city, next.weather_city) != 0 ||
                                    strcmp(cur.weather_latitude, next.weather_latitude) != 0 ||
                                    strcmp(cur.weather_longitude, next.weather_longitude) != 0 ||
                                    strcmp(cur.weather_tz, next.weather_tz) != 0;
    bool dead_battery_setting_changed = cur.dead_battery_image_enabled != next.dead_battery_image_enabled;

    cJSON_Delete(in);

    Settings::instance().update(next);
    Settings::instance().save();
    // Apply timezone immediately
    setenv("TZ", Settings::instance().timezone(), 1);
    tzset();

    user_set_device_sleep_enabled(next.device_sleep_enabled);

    // If port changed, restart server
    web_server_restart_if_needed(next.server_port);

    // Apply volume to audio player immediately
    AudioPlayer::instance().set_volume(next.audio_volume);

    if (weather_location_changed)
    {
        DataCache::instance().invalidate_weather_cache();
        user_request_refresh();
    }
    else if (dead_battery_setting_changed)
    {
        user_abort_main_delay();
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "status", "ok");
    cJSON_AddBoolToObject(out, "weather_refresh_requested", weather_location_changed);
    char *str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    send_json(req, str);
    free(str);
    return ESP_OK;
}

static esp_err_t handle_get_status(httpd_req_t *req)
{
    // Minimal status: settings + battery + rtc + wifi + timestamps + market state
    SettingsData d = Settings::instance().get();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "settings", settings_to_json(d));

    // Battery and RTC via helpers
    adc_bsp adc;
    uint8_t bat = Utilities::fetch_battery_level();
    float bat_v = adc.get_vbatVoltage();
    time_t now = time(NULL);
    struct tm tmnow;
    localtime_r(&now, &tmnow);
    char tbuf[73];
    snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmnow.tm_year + 1900, tmnow.tm_mon + 1, tmnow.tm_mday, tmnow.tm_hour, tmnow.tm_min, tmnow.tm_sec);
    cJSON_AddStringToObject(o, "rtc", tbuf);
    cJSON_AddNumberToObject(o, "battery", bat);
    cJSON_AddNumberToObject(o, "battery_voltage_v", (double)bat_v);
    size_t dead_battery_image_size = 0;
    cJSON_AddBoolToObject(o, "dead_battery_image_present", dead_battery_image_exists(&dead_battery_image_size));
    cJSON_AddNumberToObject(o, "dead_battery_image_size", (double)dead_battery_image_size);
    cJSON_AddBoolToObject(o, "dead_battery_image_test_mode", user_get_dead_battery_image_test_mode());

    // WiFi
    cJSON *w = cJSON_CreateObject();
    cJSON_AddStringToObject(w, "ssid", WiFi.SSID().c_str());
    cJSON_AddStringToObject(w, "ip", WiFi.localIP().toString().c_str());
    cJSON_AddNumberToObject(w, "rssi", WiFi.RSSI());
    cJSON_AddStringToObject(w, "bssid", WiFi.BSSIDstr().c_str());
    cJSON_AddItemToObject(o, "wifi", w);

    // Data timestamps from DataCache
    cJSON *ts = cJSON_CreateObject();
    cJSON_AddNumberToObject(ts, "stock_last_fetch", (double)DataCache::instance().get_stock_last_fetch_ts());
    //cJSON_AddNumberToObject(ts, "weather24_last_fetch", (double)DataCache::instance().get_weather24_last_fetch_ts());
    //cJSON_AddNumberToObject(ts, "weather_current_last_fetch", (double)DataCache::instance().get_weather_current_last_fetch_ts());
    cJSON_AddNumberToObject(ts, "market_last_check", (double)DataCache::instance().get_market_last_check_ts());
    cJSON_AddItemToObject(o, "timestamps", ts);

    // Market state
    bool market_open = DataCache::instance().get_market_open();
    cJSON_AddBoolToObject(o, "market_open", market_open);
    // Compute market timing details (ET regular session)
    // Use user's local timezone to display local time properly
    {
        // Get current time - CRITICAL: Do NOT set TZ environment variable in this function
        // because ESP32's gmtime_r() is buggy and gets corrupted by TZ changes
        time_t now_ts = time(nullptr);
        struct tm utc_now;
        gmtime_r(&now_ts, &utc_now);
        
        // Parse user's timezone string to get offset (e.g., "IST-2IDT-3,..." means UTC+2 standard, UTC+3 DST)
        // Format: STD_NAME offset [DST_NAME [offset] [,start[/time],end[/time]]]
        // We'll extract the standard offset from the TZ string
        const char* tz_str = Settings::instance().timezone();
        int local_offset_s = 0;  // Default to UTC
        
        if (tz_str && tz_str[0] != '\0') {
            ESP_LOGI("web_server", "[MARKET_CALC] Parsing TZ: %s", tz_str);
            // Parse format like "IST-2IDT-3,..." where -2 means UTC+2
            // Find the first digit after the timezone name
            const char *p = tz_str;
            while (*p && !(*p == '-' || *p == '+' || (*p >= '0' && *p <= '9'))) p++;
            if (*p) {
                int sign = 1;
                if (*p == '-') {
                    sign = -1;  // Negative in TZ string means positive offset (confusing POSIX convention)
                    p++;
                } else if (*p == '+') {
                    sign = 1;  // Positive in TZ string means negative offset
                    p++;
                }
                int hours = 0;
                while (*p >= '0' && *p <= '9') {
                    hours = hours * 10 + (*p - '0');
                    p++;
                }
                // TZ format: negative value means EAST of UTC (positive offset)
                local_offset_s = -sign * hours * 3600;
                
                // Check if we're in DST period for Israel (Mar-Oct roughly)
                // Israel DST: Last Friday before April 2 until last Sunday before Oct 26
                // Simplified check: if month is April-September, use DST
                int mon = utc_now.tm_mon + 1;  // 1-12
                bool is_dst = (mon >= 4 && mon <= 9);  // Rough approximation
                
                if (is_dst) {
                    // Look for DST offset in string (e.g., "IST-2IDT-3" where -3 is DST offset)
                    // Skip to DST name (usually 3 letters after first offset)
                    const char *dst_start = p;
                    while (*dst_start && !(*dst_start >= 'A' && *dst_start <= 'Z')) dst_start++;
                    if (*dst_start) {
                        // Skip DST name
                        while (*dst_start >= 'A' && *dst_start <= 'Z') dst_start++;
                        // Parse DST offset
                        if (*dst_start == '-' || *dst_start == '+' || (*dst_start >= '0' && *dst_start <= '9')) {
                            int dst_sign = 1;
                            if (*dst_start == '-') {
                                dst_sign = -1;
                                dst_start++;
                            } else if (*dst_start == '+') {
                                dst_sign = 1;
                                dst_start++;
                            }
                            int dst_hours = 0;
                            while (*dst_start >= '0' && *dst_start <= '9') {
                                dst_hours = dst_hours * 10 + (*dst_start - '0');
                                dst_start++;
                            }
                            local_offset_s = -dst_sign * dst_hours * 3600;
                        }
                    }
                }
            }
        }
        
        // Calculate local time by adding offset to UTC
        time_t local_now_ts = now_ts + local_offset_s;
        struct tm local_now;
        gmtime_r(&local_now_ts, &local_now);
        
        ESP_LOGI("web_server", "[MARKET_CALC] UTC time: %04d-%02d-%02d %02d:%02d:%02d (epoch=%ld)",
                 utc_now.tm_year+1900, utc_now.tm_mon+1, utc_now.tm_mday,
                 utc_now.tm_hour, utc_now.tm_min, utc_now.tm_sec, now_ts);
        ESP_LOGI("web_server", "[MARKET_CALC] Local time: %04d-%02d-%02d %02d:%02d:%02d (offset=%d sec = %.1f hours)",
                 local_now.tm_year+1900, local_now.tm_mon+1, local_now.tm_mday,
                 local_now.tm_hour, local_now.tm_min, local_now.tm_sec,
                 local_offset_s, local_offset_s / 3600.0);
        
        // Get ET offset for market calculations (US market operates in ET)
        auto is_dst = [](int y, int m, int d)
        {
            if (m < 3 || m > 11)
                return false;
            if (m > 3 && m < 11)
                return true;
            if (m == 3)
                return d >= 14;
            if (m == 11)
                return d < 7;
            return false;
        };
        int et_off = is_dst(utc_now.tm_year + 1900, utc_now.tm_mon + 1, utc_now.tm_mday) ? -4 : -5;
        time_t et_now_ts = now_ts + et_off * 3600;        
        struct tm et_now;
        gmtime_r(&et_now_ts, &et_now);
        
        // Lambda to convert ET time to UTC using pure arithmetic
        // et_now is already in ET timezone (from gmtime_r of et_now_ts which is now_ts + ET offset)
        // To get UTC epoch of a specific ET time on that day:
        // 1. Calculate seconds since start of day in ET: (hh*3600 + mm*60 + ss)
        // 2. Get start of day in UTC: midnight of that ET day in UTC
        //    The ET day might differ from UTC day due to timezone offset
        auto at_time_utc = [](const struct tm &et_now_ref, int hh, int mm, int ss, int et_offset_hours) -> time_t
        {
            // Start with the ET date
            struct tm utc_time = et_now_ref;
            utc_time.tm_hour = hh;
            utc_time.tm_min = mm;
            utc_time.tm_sec = ss;
            
            // Build a time_t as if utc_time were in UTC and interpret it correctly
            // We use a reference point: epoch for Jan 1, 1970 00:00:00 is 0
            // For any date (y, m, d, h, m, s), if we know the number of seconds since epoch...
            // Simpler approach: use timegm if available, otherwise manual calculation
            
            // Manual calculation: days since epoch + seconds in day
            int y = utc_time.tm_year + 1900;
            int m = utc_time.tm_mon;
            int d = utc_time.tm_mday;
            
            // Days in each month (non-leap year)
            int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            
            // Calculate days since epoch (Jan 1, 1970)
            long total_days = 0;
            
            // Add days for complete years
            for (int year = 1970; year < y; year++) {
                total_days += (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
            }
            
            // Add days for complete months in current year
            for (int month = 0; month < m; month++) {
                total_days += days_in_month[month];
                if (month == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) {
                    total_days += 1;  // leap year February
                }
            }
            
            // Add days in current month
            total_days += d - 1;  // -1 because day 1 = 0 offset
            
            // Convert to seconds and add time of day
            time_t result = total_days * 86400 + hh * 3600 + mm * 60 + ss;
            
            // Now adjust for the ET offset to get UTC
            // ET time is offset hours behind UTC, so UTC = ET + offset_hours
            // But et_offset_hours is already negative (e.g., -5), so:
            result = result - (et_offset_hours * 3600);
            
            return result;
        };
        bool weekend = (et_now.tm_wday == 0 || et_now.tm_wday == 6);
        time_t open_utc = at_time_utc(et_now, 9, 30, 0, et_off);
        time_t close_utc = at_time_utc(et_now, 16, 0, 0, et_off);
        bool in_reg = (!weekend) && difftime(now_ts, open_utc) >= 0 && difftime(close_utc, now_ts) > 0;
        
        // Detailed logging for market time calculation
        ESP_LOGI("web_server", "[MARKET_CALC] ET time: %04d-%02d-%02d %02d:%02d:%02d (wday=%d, weekend=%d)",
                 et_now.tm_year+1900, et_now.tm_mon+1, et_now.tm_mday,
                 et_now.tm_hour, et_now.tm_min, et_now.tm_sec, et_now.tm_wday, weekend);
        ESP_LOGI("web_server", "[MARKET_CALC] Open UTC: %ld (%s), Close UTC: %ld (%s)",
                 open_utc, ctime(&open_utc), close_utc, ctime(&close_utc));
        ESP_LOGI("web_server", "[MARKET_CALC] In regular hours: %d, diff now-open: %.0f, diff close-now: %.0f",
                 in_reg, difftime(now_ts, open_utc), difftime(close_utc, now_ts));
        
        cJSON *mt = cJSON_CreateObject();
        cJSON_AddNumberToObject(mt, "now_ts", (double)now_ts);
        cJSON_AddNumberToObject(mt, "now_local_ts", (double)local_now_ts);
        cJSON_AddNumberToObject(mt, "local_offset_hours", (double)local_offset_s / 3600.0);
        cJSON_AddNumberToObject(mt, "et_offset_hours", (double)et_off);
        cJSON_AddNumberToObject(mt, "regular_open_ts", (double)open_utc);
        cJSON_AddNumberToObject(mt, "regular_close_ts", (double)close_utc);
        cJSON_AddNumberToObject(mt, "is_weekend", weekend);
        cJSON_AddNumberToObject(mt, "et_hour", et_now.tm_hour);
        cJSON_AddNumberToObject(mt, "et_wday", et_now.tm_wday);
        cJSON_AddBoolToObject(mt, "in_regular", in_reg);
        if (in_reg)
        {
            double secs = difftime(close_utc, now_ts);
            if (secs < 0)
                secs = 0;
            cJSON_AddNumberToObject(mt, "seconds_until_close", secs);
        }
        else
        {
            // compute next open skipping weekend and after-close
            int add_days = 0;
            if (weekend || difftime(now_ts, close_utc) > 0)
            {
                int w = et_now.tm_wday;
                add_days = 1;
                if (w == 5)
                    add_days = 3;
                else if (w == 6)
                    add_days = 2;
                ESP_LOGI("web_server", "[MARKET_CALC] Path 1: weekend=%d or after close. w=%d, add_days=%d", 
                         weekend, w, add_days);
            }
            else if (difftime(open_utc, now_ts) > 0)
            {
                add_days = 0;
                ESP_LOGI("web_server", "[MARKET_CALC] Path 2: before open today, add_days=0");
            }
            else
            {
                ESP_LOGI("web_server", "[MARKET_CALC] Path 3: unexpected state, add_days stays 0");
            }
            
            time_t et_future = et_now_ts + add_days * 86400;
            struct tm et_f;
            gmtime_r(&et_future, &et_f);
            ESP_LOGI("web_server", "[MARKET_CALC] Future ET date: %04d-%02d-%02d %02d:%02d:%02d (add_days=%d)",
                     et_f.tm_year+1900, et_f.tm_mon+1, et_f.tm_mday,
                     et_f.tm_hour, et_f.tm_min, et_f.tm_sec, add_days);
            
            // Convert 09:30 ET that day to UTC using the same lambda
            time_t next_open_utc;
            if (add_days > 0) {
                // For future dates, use et_f
                next_open_utc = at_time_utc(et_f, 9, 30, 0, et_off);
            } else {
                // For today, use et_now
                next_open_utc = at_time_utc(et_now, 9, 30, 0, et_off);
            }
            
            ESP_LOGI("web_server", "[MARKET_CALC] Next open: et_off=%d hours, next_open_utc=%ld",
                     et_off, next_open_utc);
            ESP_LOGI("web_server", "[MARKET_CALC] Countdown: (next_open - now) = (%.0f - %.0f) = ",
                     (double)next_open_utc, (double)now_ts);
            
            cJSON_AddNumberToObject(mt, "next_open_ts", (double)next_open_utc);
            double secs = difftime(next_open_utc, now_ts);
            if (secs < 0)
                secs = 0;
            cJSON_AddNumberToObject(mt, "seconds_until_open", secs);
            ESP_LOGI("web_server", "[MARKET_CALC] Seconds until open: %.0f (%.1f hours)", secs, secs/3600.0);
        }
        cJSON_AddItemToObject(o, "market_times", mt);
    }

    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    send_json(req, str);
    free(str);
    return ESP_OK;
}

static esp_err_t handle_post_restart(httpd_req_t *req)
{
    send_json(req, "{\"status\":\"restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_post_wifi_wipe(httpd_req_t *req)
{
    WiFi.disconnect(true, true); // erase creds
    send_json(req, "{\"status\":\"wifi_wiped\"}");
    return ESP_OK;
}

static esp_err_t handle_post_cache_clear(httpd_req_t *req)
{
    // Clear all fetch-related cache data from NVS but preserve WiFi and settings
    bool success = DataCache::instance().clear_nvs_cache();
    
    if (success) {
        ESP_LOGI(TAG, "Cache cleared successfully");
        send_json(req, "{\"status\":\"ok\", \"message\":\"Cache cleared. Device will refresh data on next cycle.\"}");
    } else {
        ESP_LOGE(TAG, "Failed to clear cache");
        send_json(req, "{\"status\":\"error\", \"message\":\"Failed to clear cache\"}");
    }
    return ESP_OK;
}

static esp_err_t handle_get_wifi_info(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", WiFi.SSID().c_str());
    cJSON_AddStringToObject(o, "ip", WiFi.localIP().toString().c_str());
    cJSON_AddNumberToObject(o, "rssi", WiFi.RSSI());
    cJSON_AddStringToObject(o, "bssid", WiFi.BSSIDstr().c_str());
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    send_json(req, str);
    free(str);
    return ESP_OK;
}

static esp_err_t handle_post_sleep_toggle(httpd_req_t *req)
{
    std::string body;
    if (!read_body(req, body))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_OK;
    }
    cJSON *in = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!in)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_OK;
    }
    bool enable = parse_bool(in, "enabled", true);
    cJSON_Delete(in);
    SettingsData d = Settings::instance().get();
    d.device_sleep_enabled = enable;
    Settings::instance().update(d);
    Settings::instance().save();
    user_set_device_sleep_enabled(enable);
    send_json(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t handle_post_refresh(httpd_req_t *req)
{
    user_request_refresh();
    send_json(req, "{\"status\":\"requested\"}");
    return ESP_OK;
}

static esp_err_t handle_post_dead_battery_image_upload(httpd_req_t *req)
{
    if (req->content_len != (int)DEAD_BATTERY_IMAGE_BYTES)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image size");
        return ESP_OK;
    }

    FILE *file = fopen(DEAD_BATTERY_IMAGE_PATH, "wb");
    if (!file)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }

    int remaining = req->content_len;
    char buffer[1024];
    while (remaining > 0)
    {
        int received = httpd_req_recv(req, buffer, remaining > (int)sizeof(buffer) ? (int)sizeof(buffer) : remaining);
        if (received <= 0)
        {
            fclose(file);
            remove(DEAD_BATTERY_IMAGE_PATH);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        if (fwrite(buffer, 1, (size_t)received, file) != (size_t)received)
        {
            fclose(file);
            remove(DEAD_BATTERY_IMAGE_PATH);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_OK;
        }
        remaining -= received;
    }
    fclose(file);

    user_abort_main_delay();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "status", "ok");
    cJSON_AddNumberToObject(out, "size", (double)DEAD_BATTERY_IMAGE_BYTES);
    char *str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    send_json(req, str);
    free(str);
    return ESP_OK;
}

static esp_err_t handle_get_dead_battery_image_status(httpd_req_t *req)
{
    SettingsData settings = Settings::instance().get();
    size_t image_size = 0;
    bool image_present = dead_battery_image_exists(&image_size);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "enabled", settings.dead_battery_image_enabled);
    cJSON_AddBoolToObject(out, "uploaded", image_present);
    cJSON_AddNumberToObject(out, "size", (double)image_size);
    cJSON_AddBoolToObject(out, "test_mode", user_get_dead_battery_image_test_mode());
    char *str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    send_json(req, str);
    free(str);
    return ESP_OK;
}

static esp_err_t handle_post_dead_battery_image_test(httpd_req_t *req)
{
    std::string body;
    bool has_body = read_body(req, body);
    if (!has_body)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read body failed");
        return ESP_OK;
    }

    bool enabled = !user_get_dead_battery_image_test_mode();
    if (!body.empty())
    {
        cJSON *in = cJSON_ParseWithLength(body.c_str(), body.size());
        if (!in)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
            return ESP_OK;
        }
        enabled = parse_bool(in, "enabled", enabled);
        cJSON_Delete(in);
    }

    if (enabled && !dead_battery_image_exists())
    {
        send_json(req, "{\"status\":\"missing_image\"}");
        return ESP_OK;
    }

    user_set_dead_battery_image_test_mode(enabled);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "status", "ok");
    cJSON_AddBoolToObject(out, "test_mode", enabled);
    char *str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    send_json(req, str);
    free(str);
    return ESP_OK;
}

static esp_err_t handle_get_log(httpd_req_t *req)
{
    // Try to open log file
    FILE *f = fopen("/spiffs/log.txt", "r");
    if (!f)
    {
        // File doesn't exist - flush RAM buffer to create it
        ESP_LOGI(TAG, "Log file not found, flushing RAM buffer to create it");
        log_capture_flush();
        
        // Try again after flush
        f = fopen("/spiffs/log.txt", "r");
        if (!f)
        {
            // Still no file - return helpful message
            size_t ram_used = 0, ram_total = 0, file_size = 0;
            log_capture_stats(&ram_used, &ram_total, &file_size);
            
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            char msg[256];
            snprintf(msg, sizeof(msg), 
                     "No logs available yet.\n\n"
                     "Log buffer: %u/%u bytes used\n"
                     "Logs will appear here once the device generates output.\n",
                     (unsigned)ram_used, (unsigned)ram_total);
            httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        {
            fclose(f);
            httpd_resp_sendstr_chunk(req, nullptr);
            return ESP_OK;
        }
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

static esp_err_t handle_post_force_flush(httpd_req_t *req)
{
    log_capture_flush();
    send_json(req, "{\"status\":\"flushed\"}");
    return ESP_OK;
}

static esp_err_t handle_post_clear_log(httpd_req_t *req)
{
    if (esp_spiffs_mounted("storage"))
    {
        remove("/spiffs/log.txt");
    }
    send_json(req, "{\"status\":\"cleared\"}");
    return ESP_OK;
}

static esp_err_t handle_post_test_sound(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Test sound endpoint called");

    // Check if sounds are enabled in settings
    if (!Settings::instance().sound_enabled())
    {
        ESP_LOGW(TAG, "Test sound failed: sounds are disabled in settings");
        send_json(req, "{\"status\":\"sound_disabled\"}");
        return ESP_OK;
    }

    // Parse optional name parameter from JSON body
    std::string body;
    (void)read_body(req, body);
    const char *fallback = "market_open.mp3";
    const char *to_play = fallback;
    cJSON *in = body.empty() ? nullptr : cJSON_ParseWithLength(body.c_str(), body.size());
    if (in)
    {
        cJSON *v = cJSON_GetObjectItem(in, "name");
        if (v && cJSON_IsString(v) && name_is_allowed(v->valuestring))
        {
            to_play = v->valuestring;
        }
    }
    ESP_LOGI(TAG, "Sounds enabled, attempting to play %s", to_play);

    // Try to play the sound
    esp_err_t er = AudioPlayer::instance().play(to_play, false);
    if (er != ESP_OK)
    {
        ESP_LOGE(TAG, "Test sound playback failed with error: %s", esp_err_to_name(er));
        send_json(req, "{\"status\":\"error\",\"message\":\"playback_failed\"}");
        return ESP_OK;
    }

    if (in)
        cJSON_Delete(in);
    ESP_LOGI(TAG, "Test sound playback initiated successfully");
    send_json(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t handle_post_test_tone(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Test tone endpoint called");
    // 1-second 440 Hz at current volume
    audio_play_tone(440, 1000, Settings::instance().audio_volume());
    send_json(req, "{\"status\":\"ok\",\"tone\":440}");
    return ESP_OK;
}
