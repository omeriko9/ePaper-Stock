#include "power_mgmt.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_rom_gpio.h"
#include <nvs.h>

#include "user_config.h"
#include "i2c_bsp.h"
#include "adc_bsp.h"

static const char *TAG = "power";

static const char *NVS_NS = "power";
static const char *NVS_KEY_LATCH = "latch";

enum latch_mode_t : uint8_t
{
    LATCH_PULLUP = 0, // weak internal pull-up in sleep (~0.06 mA)
    LATCH_STRONG = 1, // output high in sleep (~2.5 mA), always works
};

// Kept across deep sleep so timer wakes do not need NVS for it.
RTC_DATA_ATTR static uint8_t s_latch_mode = LATCH_PULLUP;

// PCF85063 general-purpose RAM byte. The RTC chip is powered from VBAT, so the
// byte survives when the ESP32 loses power. We set it while we sleep with the weak
// latch; if the next boot is a power-on with the byte still set, the latch let go.
static constexpr uint8_t PCF85063_REG_CONTROL_2 = 0x01;
static constexpr uint8_t PCF85063_REG_RAM_BYTE = 0x03;
static constexpr uint8_t PCF85063_COF_MASK = 0x07; // COF = 111: CLKOUT off
static constexpr uint8_t LATCH_ARMED_MARKER = 0xA5;

// Below this battery voltage a power-on is more likely a dead battery than a latch fault.
static constexpr float LATCH_FAULT_MIN_VBAT = 3.5f;

// RTC-capable pads with nothing to drive in deep sleep. Isolation removes pulls and
// input/output paths, so no current flows into the unpowered e-paper.
// GPIO6 (EPD power enable) and the LED on GPIO3 have external pull-ups to 3V3, so a
// floating pad turns them off.
static const gpio_num_t k_isolate_in_sleep[] = {
    GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6,
    GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12,
    GPIO_NUM_13, GPIO_NUM_16, GPIO_NUM_21,
};

// The audio rail stays on in deep sleep (see power_enter_deep_sleep), so the codec
// inputs must not float: MCLK, SCLK (RTC pads), LRCK, DSDIN and PA_CTRL (digital pads).
static const gpio_num_t k_pulldown_rtc_in_sleep[] = {GPIO_NUM_14, GPIO_NUM_15};
static const gpio_num_t k_pulldown_digital_in_sleep[] = {GPIO_NUM_38, GPIO_NUM_45, GPIO_NUM_46};

static void latch_drive_strong(void)
{
    const gpio_num_t pin = (gpio_num_t)VBAT_PWR_PIN;
    // Prepare the digital output before the pad leaves RTC mode and its hold,
    // so the latch never sees a low level.
    gpio_set_level(pin, 1);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    rtc_gpio_deinit(pin);
    gpio_hold_dis(pin);
}

static void audio_rail_on(void)
{
    // P-FET switch, low = on. GPIO42 starts in its JTAG function (MTMS), so select
    // the GPIO function first; without that the output has no effect.
    const gpio_num_t pin = (gpio_num_t)Audio_PWR_PIN;
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_hold_dis(pin);
}

void power_init_on_boot(void)
{
    latch_drive_strong();
    audio_rail_on();

    for (gpio_num_t pin : k_isolate_in_sleep)
    {
        gpio_hold_dis(pin);
        rtc_gpio_deinit(pin);
    }
    for (gpio_num_t pin : k_pulldown_rtc_in_sleep)
    {
        gpio_hold_dis(pin);
        rtc_gpio_deinit(pin);
    }
    for (gpio_num_t pin : k_pulldown_digital_in_sleep)
    {
        gpio_hold_dis(pin);
    }
}

static uint8_t latch_mode_load(void)
{
    uint8_t mode = LATCH_PULLUP;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK)
    {
        nvs_get_u8(nvs, NVS_KEY_LATCH, &mode);
        nvs_close(nvs);
    }
    return mode;
}

static void latch_mode_save(uint8_t mode)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_u8(nvs, NVS_KEY_LATCH, mode);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void power_latch_check_on_cold_boot(esp_reset_reason_t reset_reason)
{
    s_latch_mode = latch_mode_load();

    if (rtc_dev_handle == nullptr)
    {
        // Without the detector we cannot recover from a fault, so do not take the risk.
        ESP_LOGW(TAG, "RTC chip not available - strong latch drive in sleep");
        s_latch_mode = LATCH_STRONG;
        return;
    }

    uint8_t marker = 0;
    bool marker_ok = (i2c_read_buff(rtc_dev_handle, PCF85063_REG_RAM_BYTE, &marker, 1) == ESP_OK);

    if (marker_ok && marker == LATCH_ARMED_MARKER && reset_reason == ESP_RST_POWERON &&
        s_latch_mode == LATCH_PULLUP)
    {
        adc_bsp adc;
        float vbat = adc.get_vbatVoltage();
        if (vbat > LATCH_FAULT_MIN_VBAT)
        {
            ESP_LOGW(TAG, "Board lost power during a pull-up sleep (VBAT %.2fV) - using strong latch drive from now on", vbat);
            s_latch_mode = LATCH_STRONG;
            latch_mode_save(s_latch_mode);
        }
        else
        {
            ESP_LOGI(TAG, "Power-on after low battery (VBAT %.2fV) - keeping pull-up latch", vbat);
        }
    }

    uint8_t new_marker = (s_latch_mode == LATCH_PULLUP) ? LATCH_ARMED_MARKER : 0;
    if (!marker_ok || new_marker != marker)
    {
        if (i2c_write_buff(rtc_dev_handle, PCF85063_REG_RAM_BYTE, &new_marker, 1) != ESP_OK)
        {
            ESP_LOGW(TAG, "Cannot arm latch detector - strong latch drive in sleep");
            s_latch_mode = LATCH_STRONG;
        }
    }

    ESP_LOGI(TAG, "Latch mode in sleep: %s", s_latch_mode == LATCH_PULLUP ? "pull-up" : "strong");
}

void power_quiet_i2c_peripherals(void)
{
    // SHTC3 starts in idle mode (~45 uA) after power-up. This firmware does not use it.
    if (shtc3_handle != nullptr)
    {
        uint8_t sleep_cmd[2] = {0xB0, 0x98};
        if (i2c_write_buff(shtc3_handle, -1, sleep_cmd, sizeof(sleep_cmd)) == ESP_OK)
        {
            ESP_LOGI(TAG, "SHTC3 put to sleep");
        }
    }

    // PCF85063 outputs 32.768 kHz on CLKOUT by default. The pin is not connected.
    if (rtc_dev_handle != nullptr)
    {
        uint8_t ctrl2 = 0;
        if (i2c_read_buff(rtc_dev_handle, PCF85063_REG_CONTROL_2, &ctrl2, 1) == ESP_OK &&
            (ctrl2 & PCF85063_COF_MASK) != PCF85063_COF_MASK)
        {
            ctrl2 |= PCF85063_COF_MASK;
            i2c_write_buff(rtc_dev_handle, PCF85063_REG_CONTROL_2, &ctrl2, 1);
            ESP_LOGI(TAG, "RTC CLKOUT disabled");
        }
    }
}

static void latch_prepare_for_sleep(void)
{
    const gpio_num_t pin = (gpio_num_t)VBAT_PWR_PIN;
    // Set up the RTC side first; the pad switches to it in rtc_gpio_init().
    rtc_gpio_set_level(pin, 1);
    if (s_latch_mode == LATCH_PULLUP)
    {
        rtc_gpio_pullup_en(pin);
        rtc_gpio_pulldown_dis(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    }
    else
    {
        rtc_gpio_pullup_dis(pin);
        rtc_gpio_pulldown_dis(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_OUTPUT_ONLY);
    }
    rtc_gpio_init(pin);
    rtc_gpio_hold_en(pin);
}

void power_enter_deep_sleep(uint64_t sleep_us)
{
    for (gpio_num_t pin : k_isolate_in_sleep)
    {
        rtc_gpio_isolate(pin);
    }
    for (gpio_num_t pin : k_pulldown_rtc_in_sleep)
    {
        rtc_gpio_init(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_DISABLED);
        rtc_gpio_pullup_dis(pin);
        rtc_gpio_pulldown_en(pin);
        rtc_gpio_hold_en(pin);
    }
    for (gpio_num_t pin : k_pulldown_digital_in_sleep)
    {
        esp_rom_gpio_pad_select_gpio(pin);
        gpio_set_direction(pin, GPIO_MODE_DISABLE);
        gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        gpio_hold_en(pin);
    }

    // Keep the audio rail on. The ES8311 codec sits on the I2C bus; without power it
    // clamps SDA and SCL, and the two 4.7k pull-ups then feed about 1 mA into it.
    // The codec is in standby here (AudioPlayer::deinit) or still in its reset state.
    gpio_set_level((gpio_num_t)Audio_PWR_PIN, 0);
    gpio_hold_en((gpio_num_t)Audio_PWR_PIN);
    gpio_deep_sleep_hold_en();

    latch_prepare_for_sleep();
    // Pad pulls and holds live in the RTC peripheral domain.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    if (sleep_us > 0)
    {
        esp_sleep_enable_timer_wakeup(sleep_us);
    }
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << BOOT_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BOOT button wakeup not available: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Deep sleep for %llu s (latch %s)", sleep_us / 1000000ULL,
             s_latch_mode == LATCH_PULLUP ? "pull-up" : "strong");

    // The ROM boot banner on every wake costs time on the console.
    esp_deep_sleep_disable_rom_logging();
    esp_deep_sleep_start();
}
