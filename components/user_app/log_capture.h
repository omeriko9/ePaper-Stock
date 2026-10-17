#pragma once

#include <esp_err.h>

/**
 * Simple log capture system that intercepts ESP_LOGx calls and saves them to /spiffs/log.txt
 * 
 * Features:
 * - Buffers logs in RAM to minimize SPIFFS writes
 * - Auto-flushes when buffer is 80% full
 * - Manual flush via log_capture_flush()
 * - Keeps last ~1500 lines through rotation
 * - Thread-safe with spinlock protection
 * - ISR-safe (drops ISR logs to prevent watchdog)
 */

/**
 * Initialize the log capture system
 * Must be called after SPIFFS is mounted
 * 
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t log_capture_init(void);

/**
 * Force immediate flush of RAM buffer to file
 * Safe to call from any context (non-ISR)
 */
void log_capture_flush(void);

/**
 * Get statistics about the logging system
 * 
 * @param ram_used Output: bytes currently in RAM buffer
 * @param ram_total Output: total RAM buffer size
 * @param file_size Output: current log file size (0 if doesn't exist)
 */
void log_capture_stats(size_t *ram_used, size_t *ram_total, size_t *file_size);
