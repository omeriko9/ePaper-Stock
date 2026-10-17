#pragma once

#include <esp_err.h>
#include <stdint.h>
#include "driver/i2c_master.h"

/**
 * @brief ES8311 Audio Codec Driver
 * 
 * Low-power audio codec with I2C control and I2S data interface.
 * Used in many ESP32 audio boards.
 */

#define ES8311_I2C_ADDR 0x18  // 7-bit I2C address from schematic

// ES8311 Register Addresses
#define ES8311_RESET_REG00             0x00
#define ES8311_CLK_MANAGER_REG01       0x01
#define ES8311_CLK_MANAGER_REG02       0x02
#define ES8311_CLK_MANAGER_REG03       0x03
#define ES8311_CLK_MANAGER_REG04       0x04
#define ES8311_CLK_MANAGER_REG05       0x05
#define ES8311_CLK_MANAGER_REG06       0x06
#define ES8311_CLK_MANAGER_REG07       0x07
#define ES8311_CLK_MANAGER_REG08       0x08
#define ES8311_SDPIN_REG09             0x09
#define ES8311_SYSTEM_REG0A            0x0A
#define ES8311_SYSTEM_REG0B            0x0B
#define ES8311_SYSTEM_REG0C            0x0C
#define ES8311_SYSTEM_REG0D            0x0D
#define ES8311_SYSTEM_REG0E            0x0E
#define ES8311_SYSTEM_REG0F            0x0F
#define ES8311_SYSTEM_REG10            0x10
#define ES8311_SYSTEM_REG11            0x11
#define ES8311_SYSTEM_REG12            0x12
#define ES8311_SYSTEM_REG13            0x13
#define ES8311_SYSTEM_REG14            0x14
#define ES8311_ADC_REG15               0x15
#define ES8311_ADC_REG16               0x16
#define ES8311_ADC_REG17               0x17
#define ES8311_ADC_REG18               0x18
#define ES8311_ADC_REG19               0x19
#define ES8311_ADC_REG1A               0x1A
#define ES8311_ADC_REG1B               0x1B
#define ES8311_ADC_REG1C               0x1C
#define ES8311_DAC_REG31               0x31
#define ES8311_DAC_REG32               0x32
#define ES8311_DAC_REG33               0x33
#define ES8311_DAC_REG34               0x34
#define ES8311_DAC_REG35               0x35
#define ES8311_DAC_REG36               0x36
#define ES8311_DAC_REG37               0x37
#define ES8311_GPIO_REG44              0x44
#define ES8311_GP_REG45                0x45

class ES8311 {
public:
    static ES8311& instance();

    /**
     * @brief Initialize ES8311 codec via I2C
     * @param dev_handle I2C device handle (new driver API)
     * @param sample_rate Audio sample rate (e.g., 48000)
     * @return ESP_OK on success
     */
    esp_err_t init(i2c_master_dev_handle_t dev_handle, uint32_t sample_rate);

    /**
     * @brief Set DAC output volume
     * @param volume Volume level 0-100 (0=mute, 100=max)
     * @return ESP_OK on success
     */
    esp_err_t set_volume(uint8_t volume);

    /**
     * @brief Get current volume
     * @return Volume level 0-100
     */
    uint8_t get_volume();

    /**
     * @brief Mute/unmute DAC output
     * @param mute true=mute, false=unmute
     * @return ESP_OK on success
     */
    esp_err_t set_mute(bool mute);

    /**
     * @brief Power down codec (low power mode)
     * @return ESP_OK on success
     */
    esp_err_t power_down();

    /**
     * @brief Power up codec from low power mode
     * @return ESP_OK on success
     */
    esp_err_t power_up();

private:
    ES8311() : m_dev_handle(nullptr), m_volume(70), m_initialized(false) {}
    ES8311(const ES8311&) = delete;
    ES8311& operator=(const ES8311&) = delete;

    esp_err_t write_reg(uint8_t reg, uint8_t value);
    esp_err_t read_reg(uint8_t reg, uint8_t* value);

    i2c_master_dev_handle_t m_dev_handle;
    uint8_t m_volume;
    bool m_initialized;
};
