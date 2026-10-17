#include "log_capture.h"
#include "settings.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include <time.h>
#include <sys/stat.h>
#include <esp_attr.h>
#include <rom/rtc.h>

static const char* TAG = "logcap";

// Configuration
static const size_t RAM_BUFFER_SIZE = 256 * 1024;  // 256KB in PSRAM
static const size_t MAX_LOG_FILE_SIZE = 150 * 1024; // 150KB (~1500 lines)
static const float FLUSH_THRESHOLD = 0.80f;         // Flush at 80% full
static const char* LOG_FILE_PATH = "/spiffs/log.txt";
static const char* LOG_TEMP_PATH = "/spiffs/log_tmp.txt";

// State
static char* s_ram_buffer = nullptr;
static std::atomic<size_t> s_ram_used{0};
static vprintf_like_t s_original_vprintf = nullptr;
static std::atomic<bool> s_flushing{false};
static portMUX_TYPE s_spinlock = portMUX_INITIALIZER_UNLOCKED;

/**
 * Internal vprintf hook - intercepts all ESP_LOGx output
 */
static int vprintf_hook(const char* fmt, va_list args)
{
    // Safety: Drop logs from ISR context to prevent watchdog issues
    if (xPortInIsrContext()) {
        return 0;
    }

    // Safety: If we're currently flushing, just send to serial to avoid recursion
    if (s_flushing.load()) {
        return s_original_vprintf ? s_original_vprintf(fmt, args) : 0;
    }

    // Safety: If buffer not initialized, fall back to serial only
    if (!s_ram_buffer) {
        return s_original_vprintf ? s_original_vprintf(fmt, args) : 0;
    }

    // Check if log capture is enabled in settings
    if (!Settings::instance().log_capture_enabled()) {
        return s_original_vprintf ? s_original_vprintf(fmt, args) : 0;
    }

    // Copy args for reuse (vprintf consumes them)
    va_list args_copy;
    va_copy(args_copy, args);

    // Always output to serial (original behavior)
    int serial_result = s_original_vprintf ? s_original_vprintf(fmt, args) : 0;

    // Build timestamped log entry - format: [YYYY-MM-DD HH:MM:SS]: log text
    char timestamp[100];  // Increased size to avoid truncation warning
    timestamp[0] = '\0';
    time_t now = time(NULL);
    if (now > 1577836800) { // After Jan 1, 2020 - time is set
        struct tm timeinfo;
        if (localtime_r(&now, &timeinfo)) {
            snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d]: ",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        }
    }

    // Critical section: append to RAM buffer
    portENTER_CRITICAL(&s_spinlock);
    
    size_t used = s_ram_used.load();
    size_t available = RAM_BUFFER_SIZE - used;
    
    if (available > 1) {
        // Add timestamp first
        size_t ts_len = strlen(timestamp);
        if (ts_len > 0 && ts_len < available) {
            memcpy(s_ram_buffer + used, timestamp, ts_len);
            used += ts_len;
            available -= ts_len;
        }
        
        // Add log message
        int written = vsnprintf(s_ram_buffer + used, available, fmt, args_copy);
        if (written > 0 && (size_t)written < available) {
            s_ram_used.store(used + written);
        } else if (written > 0) {
            // Buffer full - mark it
            s_ram_used.store(RAM_BUFFER_SIZE);
        }
    }
    
    size_t current_used = s_ram_used.load();
    portEXIT_CRITICAL(&s_spinlock);

    va_end(args_copy);

    // Auto-flush if buffer is getting full (outside critical section)
    if (current_used >= (size_t)(RAM_BUFFER_SIZE * FLUSH_THRESHOLD)) {
        log_capture_flush();
    }

    return serial_result;
}

/**
 * Rotate log file: keep newest data when file exceeds MAX_LOG_FILE_SIZE
 * Uses line-boundary rotation to avoid partial lines
 */
static void rotate_log_file(const char* new_data, size_t new_len)
{
    FILE* old_file = fopen(LOG_FILE_PATH, "rb");
    size_t old_size = 0;
    
    if (old_file) {
        fseek(old_file, 0, SEEK_END);
        old_size = ftell(old_file);
        fseek(old_file, 0, SEEK_SET);
    }

    // If total size fits, just append
    if (old_size + new_len <= MAX_LOG_FILE_SIZE) {
        if (old_file) fclose(old_file);
        
        FILE* f = fopen(LOG_FILE_PATH, "ab");
        if (f) {
            fwrite(new_data, 1, new_len, f);
            fclose(f);
        }
        return;
    }

    // Need to rotate: keep newest portion
    FILE* temp_file = fopen(LOG_TEMP_PATH, "wb");
    if (!temp_file) {
        if (old_file) fclose(old_file);
        ESP_LOGE(TAG, "Failed to create temp file for rotation");
        return;
    }

    if (old_file) {
        // Calculate how much old data to skip
        size_t total_size = old_size + new_len;
        size_t skip_bytes = total_size - MAX_LOG_FILE_SIZE;
        
        if (skip_bytes >= old_size) {
            // Skip entire old file, only keep new data
            fclose(old_file);
            old_file = nullptr;
        } else {
            // Seek to skip point in old file
            fseek(old_file, skip_bytes, SEEK_SET);
            
            // Advance to next newline to respect line boundaries
            int ch;
            while ((ch = fgetc(old_file)) != EOF && ch != '\n') {
                // skip to newline
            }
            
            // Copy remainder of old file to temp
            char buf[512];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), old_file)) > 0) {
                fwrite(buf, 1, n, temp_file);
            }
            
            fclose(old_file);
            old_file = nullptr;
        }
    }

    // Append new data
    fwrite(new_data, 1, new_len, temp_file);
    fclose(temp_file);

    // Atomic replace
    remove(LOG_FILE_PATH);
    rename(LOG_TEMP_PATH, LOG_FILE_PATH);
}

void log_capture_flush(void)
{
    // Prevent re-entry
    if (s_flushing.exchange(true)) {
        return; // Already flushing
    }

    // Copy buffer contents under lock
    portENTER_CRITICAL(&s_spinlock);
    size_t len = s_ram_used.load();
    if (len == 0) {
        portEXIT_CRITICAL(&s_spinlock);
        s_flushing = false;
        return;
    }

    char* temp_buf = (char*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!temp_buf) {
        ESP_LOGE(TAG, "Failed to allocate temp buffer for flush");
        portEXIT_CRITICAL(&s_spinlock);
        s_flushing = false;
        return;
    }

    memcpy(temp_buf, s_ram_buffer, len);
    s_ram_used = 0; // Clear buffer
    portEXIT_CRITICAL(&s_spinlock);

    // Write to file (outside lock to avoid blocking)
    rotate_log_file(temp_buf, len);

    heap_caps_free(temp_buf);
    s_flushing = false;
}

esp_err_t log_capture_init(void)
{
    if (s_ram_buffer) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Allocate buffer in PSRAM to save internal RAM
    s_ram_buffer = (char*)heap_caps_malloc(RAM_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_ram_buffer) {
        ESP_LOGE(TAG, "Failed to allocate %u byte buffer in PSRAM", (unsigned)RAM_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }

    s_ram_used = 0;

    // Hook into ESP logging system BEFORE writing restart marker
    s_original_vprintf = esp_log_set_vprintf(vprintf_hook);

    // Write restart marker with timestamp and reset reason to help debug crashes
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char* reset_str = "UNKNOWN";
    switch (reset_reason) {
        case ESP_RST_POWERON: reset_str = "POWERON"; break;
        case ESP_RST_EXT: reset_str = "EXT"; break;
        case ESP_RST_SW: reset_str = "SW"; break;
        case ESP_RST_PANIC: reset_str = "PANIC"; break;
        case ESP_RST_INT_WDT: reset_str = "INT_WDT"; break;
        case ESP_RST_TASK_WDT: reset_str = "TASK_WDT"; break;
        case ESP_RST_WDT: reset_str = "WDT"; break;
        case ESP_RST_DEEPSLEEP: reset_str = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT: reset_str = "BROWNOUT"; break;
        case ESP_RST_SDIO: reset_str = "SDIO"; break;
        default: break;
    }

    // Log the restart event - this will go through our vprintf_hook
    ESP_LOGI(TAG, "========== DEVICE RESTART: %s ==========", reset_str);
    
    // Get additional crash details from RTC memory if available
    RESET_REASON rtc_reset_reason = rtc_get_reset_reason(0);
    RESET_REASON rtc_reset_reason_1 = rtc_get_reset_reason(1);
    
    if (reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_INT_WDT || 
        reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT) {
        
        ESP_LOGW(TAG, "=== CRASH DETECTED ===");
        ESP_LOGI(TAG, "RTC Reset Reason: CPU0=%d CPU1=%d", rtc_reset_reason, rtc_reset_reason_1);
        ESP_LOGI(TAG, "Free heap: %lu bytes (internal: %lu, PSRAM: %lu)",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGI(TAG, "Min free heap ever: %lu bytes", (unsigned long)esp_get_minimum_free_heap_size());
        
        // Explain how to get full crash details
        ESP_LOGW(TAG, "*** IMPORTANT: Full crash details (registers, backtrace) were printed to serial at panic time ***");
        ESP_LOGW(TAG, "*** To capture those details automatically, enable CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH in menuconfig ***");
        ESP_LOGW(TAG, "*** Run: idf.py menuconfig -> Component config -> Core dump -> Data destination: Flash ***");
        ESP_LOGW(TAG, "=== END CRASH INFO ===");
    }
    
    ESP_LOGI(TAG, "Log capture initialized: %u KB RAM buffer, %u KB max file",
             (unsigned)(RAM_BUFFER_SIZE / 1024), (unsigned)(MAX_LOG_FILE_SIZE / 1024));

    return ESP_OK;
}

void log_capture_stats(size_t* ram_used, size_t* ram_total, size_t* file_size)
{
    if (ram_used) {
        *ram_used = s_ram_used.load();
    }
    if (ram_total) {
        *ram_total = RAM_BUFFER_SIZE;
    }
    if (file_size) {
        struct stat st;
        *file_size = (stat(LOG_FILE_PATH, &st) == 0) ? st.st_size : 0;
    }
}
