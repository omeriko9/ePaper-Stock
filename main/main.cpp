#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <Arduino.h>
#include <WiFi.h>

#include "user_config.h"
#include "user_app.h"
#include "board_power_bsp.h"
#include "power_mgmt.h"
#include "ePaper.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"

static const char *TAG = "main_1_54";

extern i2c_equipment *rtc_dev;

// The esp_diagnostics component links with --wrap=esp_log_write/esp_log_writev, but this
// build does not include its log hook. These empty versions satisfy the link. They also
// mute the libraries that call esp_log_write directly (the WiFi blobs).
extern "C" void __wrap_esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{
}

extern "C" void __wrap_esp_log_writev(esp_log_level_t level, const char *tag, const char *format, va_list args)
{
}

// Returns "[YYYY-MM-DD HH:MM:SS] " in local time, or "" while the clock is not set.
static const char *get_rtc_timestamp()
{
    static char timestamp[64];
    timestamp[0] = '\0';

    time_t now = time(NULL);
    if (now < 1577836800) // Jan 1, 2020: clock not set yet
    {
        return timestamp;
    }

    struct tm timeinfo;
    if (localtime_r(&now, &timeinfo) != NULL)
    {
        snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    return timestamp;
}

static const char *reset_reason_name(esp_reset_reason_t rr)
{
    switch (rr)
    {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "UNKNOWN";
    }
}

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

// Apply the persisted global log level (INFO if not set).
static void apply_saved_log_level(void)
{
    uint8_t lvl = (uint8_t)ESP_LOG_INFO;
    nvs_handle_t nvs;
    if (nvs_open("config", NVS_READONLY, &nvs) == ESP_OK)
    {
        nvs_get_u8(nvs, "log_lvl", &lvl);
        nvs_close(nvs);
    }
    esp_log_level_set("*", (esp_log_level_t)lvl);
}

// Keep the last reset reason in NVS for post-mortem analysis.
static void save_reset_reason(esp_reset_reason_t rr)
{
    nvs_handle_t nvs;
    if (nvs_open("diag", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_u8(nvs, "last_reset_reason", (uint8_t)rr);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void show_loading_at_startup()
{
    EPaper &ep = EPaper::get_instance();
    ep.init();
    ep.clear_buffer();
    ep.draw_text5x7_center(90, "Loading...", 2);
    ep.display_full_refresh();
}

extern "C" void app_main(void)
{
    // First: keep the battery latch on, turn the audio rail on (the I2C bus needs it)
    // and take back the pads that deep sleep held.
    power_init_on_boot();

    initArduino();

    const esp_reset_reason_t rr = esp_reset_reason();
    const bool cold_boot = (rr != ESP_RST_DEEPSLEEP);

    board_power_bsp_t board_div(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
    board_div.POWER_EPD_ON();

    init_nvs();
    apply_saved_log_level();
    ESP_LOGI(TAG, "Reset reason: %s", reset_reason_name(rr));

    // The RTC chip keeps better time in deep sleep than the ESP32's internal RC clock,
    // so read it on every boot (user_app_init sets the system time from it).
    i2c_master_Init();
    rtc_dev = new i2c_equipment();

    if (cold_boot)
    {
        // Deep sleep wakes always write the same value; skip the NVS write for them.
        save_reset_reason(rr);

        power_latch_check_on_cold_boot(rr);
        power_quiet_i2c_peripherals();

        show_loading_at_startup();
    }

    // Core netif and event loop must be ready before any WiFi operation.
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_create_default());

    WiFi.setSleep(true);         // modem sleep while associated
    WiFi.persistent(true);       // store credentials
    WiFi.setAutoReconnect(true);

    user_app_init(rr);
    user_ui_init(); // starts the main loop task

    ESP_LOGI(TAG, "Time after wake up/init: %s", get_rtc_timestamp());
}
