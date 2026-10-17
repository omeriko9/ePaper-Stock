#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "board_power_bsp.h"
#include "esp_err.h"

board_power_bsp_t::board_power_bsp_t(uint8_t _epd_power_pin, uint8_t _audio_power_pin, uint8_t _vbat_power_pin, bool should_enable_audio) : epd_power_pin(_epd_power_pin),
                                                                                                                                            audio_power_pin(_audio_power_pin),
                                                                                                                                            vbat_power_pin(_vbat_power_pin)
{
    // Reset and configure EPD power pin
    ESP_ERROR_CHECK(gpio_reset_pin((gpio_num_t)epd_power_pin));
    
    gpio_config_t epd_conf = {};
    epd_conf.intr_type = GPIO_INTR_DISABLE;
    epd_conf.mode = GPIO_MODE_OUTPUT;
    epd_conf.pin_bit_mask = (1ULL << epd_power_pin);
    epd_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    epd_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&epd_conf));

    // The audio rail (audio_power_pin) is set up by power_mgmt before this runs. Resetting
    // the pad here would turn the rail off and take the I2C bus down with the codec.
    (void)should_enable_audio;

    // The battery latch pin (vbat_power_pin) belongs to power_mgmt; do not touch it here.
}

board_power_bsp_t::~board_power_bsp_t()
{
}

void board_power_bsp_t::POWER_EPD_ON()
{
    gpio_set_level((gpio_num_t)epd_power_pin, 0);
}

void board_power_bsp_t::POWER_EPD_OFF()
{
    gpio_set_level((gpio_num_t)epd_power_pin, 1);
}

void board_power_bsp_t::POWER_Audio_ON()
{
    gpio_set_level((gpio_num_t)audio_power_pin, 0);
}

void board_power_bsp_t::POWER_Audio_OFF()
{
    gpio_set_level((gpio_num_t)audio_power_pin, 1);
}
