#include "es8311.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ES8311";

ES8311& ES8311::instance() {
    static ES8311 inst;
    return inst;
}

esp_err_t ES8311::write_reg(uint8_t reg, uint8_t value) {
    if (!m_dev_handle) {
        ESP_LOGE(TAG, "I2C device not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t write_buf[2] = {reg, value};
    esp_err_t ret = i2c_master_transmit(m_dev_handle, write_buf, 2, pdMS_TO_TICKS(1000));
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: reg=0x%02X, val=0x%02X, err=%s", reg, value, esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t ES8311::read_reg(uint8_t reg, uint8_t* value) {
    if (!m_dev_handle || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_master_transmit_receive(m_dev_handle, &reg, 1, value, 1, pdMS_TO_TICKS(1000));
    
    return ret;
}

esp_err_t ES8311::init(i2c_master_dev_handle_t dev_handle, uint32_t sample_rate) {
    m_dev_handle = dev_handle;
    
    if (!m_dev_handle) {
        ESP_LOGE(TAG, "Invalid I2C device handle");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing ES8311 codec at I2C address 0x%02X", ES8311_I2C_ADDR);
    
    // Reset codec
    ESP_LOGI(TAG, "Resetting codec...");
    write_reg(ES8311_RESET_REG00, 0x1F);  // Reset all
    write_reg(ES8311_RESET_REG00, 0x00);  // Release reset
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // Clock configuration for 48kHz with MCLK=12.288MHz
    ESP_LOGI(TAG, "Configuring clocks for %lu Hz sample rate...", sample_rate);
    write_reg(ES8311_CLK_MANAGER_REG01, 0x3F);  // MCLK divider = 1
    write_reg(ES8311_CLK_MANAGER_REG02, 0x00);  // Internal MCLK
    write_reg(ES8311_CLK_MANAGER_REG03, 0x10);  // BCLK divider
    write_reg(ES8311_CLK_MANAGER_REG04, 0x10);  // LRCK divider high
    write_reg(ES8311_CLK_MANAGER_REG05, 0x00);  // LRCK divider low
    write_reg(ES8311_CLK_MANAGER_REG06, 0x00);  // ADC/DAC clock source
    write_reg(ES8311_CLK_MANAGER_REG07, 0x00);  // DAC MCLK
    write_reg(ES8311_CLK_MANAGER_REG08, 0x00);  // ADC MCLK
    
    // I2S configuration
    ESP_LOGI(TAG, "Configuring I2S interface...");
    write_reg(ES8311_SDPIN_REG09, 0x00);        // I2S normal mode
    write_reg(ES8311_SYSTEM_REG0A, 0x00);       // Normal polarity
    write_reg(ES8311_SYSTEM_REG0B, 0x00);       // I2S 16-bit
    write_reg(ES8311_SYSTEM_REG0C, 0x00);       // I2S slave mode
    write_reg(ES8311_SYSTEM_REG0D, 0x01);       // MCLK from pin
    write_reg(ES8311_SYSTEM_REG0E, 0x02);       // Power up analog
    write_reg(ES8311_SYSTEM_REG0F, 0x44);       // Power up DAC
    write_reg(ES8311_SYSTEM_REG10, 0x00);       // Reference voltage
    write_reg(ES8311_SYSTEM_REG11, 0x00);       // Power down control
    write_reg(ES8311_SYSTEM_REG12, 0x02);       // DAC sample rate config
    write_reg(ES8311_SYSTEM_REG13, 0x00);       // Normal operation
    write_reg(ES8311_SYSTEM_REG14, 0x00);       // Normal bias
    
    // DAC configuration
    ESP_LOGI(TAG, "Configuring DAC...");
    write_reg(ES8311_DAC_REG31, 0x00);          // DAC normal
    write_reg(ES8311_DAC_REG32, 0xBF);          // DAC volume (default)
    write_reg(ES8311_DAC_REG33, 0x00);          // DAC ramp rate
    write_reg(ES8311_DAC_REG34, 0x00);          // DAC HPF
    write_reg(ES8311_DAC_REG35, 0x00);          // DAC de-emphasis
    write_reg(ES8311_DAC_REG36, 0x00);          // DAC offset
    write_reg(ES8311_DAC_REG37, 0x08);          // DAC L/R channel control
    
    // GPIO configuration (if needed)
    write_reg(ES8311_GPIO_REG44, 0x08);         // GPIO config
    write_reg(ES8311_GP_REG45, 0x00);           // GP config
    
    // Power up and unmute
    ESP_LOGI(TAG, "Powering up codec...");
    write_reg(ES8311_SYSTEM_REG0D, 0x01);       // Power on
    write_reg(ES8311_DAC_REG31, 0x60);          // Enable DAC, unmute
    write_reg(ES8311_GP_REG45, 0x00);           // GPIO normal
    
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Set default volume (70%)
    set_volume(m_volume);
    
    m_initialized = true;
    ESP_LOGI(TAG, "ES8311 initialized successfully at %lu Hz, volume=%d%%", sample_rate, m_volume);
    
    return ESP_OK;
}

esp_err_t ES8311::set_volume(uint8_t volume) {
    if (!m_initialized) {
        ESP_LOGW(TAG, "Codec not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (volume > 100) {
        volume = 100;
    }
    
    m_volume = volume;
    
    // ES8311 DAC volume register: 0x00 = max, 0xFF = min
    // Map 0-100% to 0xFF-0x00 (inverted)
    // Using a range of 0xC0-0x00 for better control (0xC0 is very quiet, 0xFF is mute)
    uint8_t reg_value;
    if (volume == 0) {
        reg_value = 0xFF;  // Mute
    } else {
        // Map 1-100% to 0xBF-0x00 (191 steps)
        reg_value = 0xBF - ((uint32_t)volume * 191 / 100);
    }
    
    ESP_LOGI(TAG, "Setting volume: %d%% (reg=0x%02X)", volume, reg_value);
    return write_reg(ES8311_DAC_REG32, reg_value);
}

uint8_t ES8311::get_volume() {
    return m_volume;
}

esp_err_t ES8311::set_mute(bool mute) {
    if (!m_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "%s DAC output", mute ? "Muting" : "Unmuting");
    
    if (mute) {
        return write_reg(ES8311_DAC_REG31, 0x20);  // Mute DAC
    } else {
        return write_reg(ES8311_DAC_REG31, 0x60);  // Unmute DAC, enable
    }
}

esp_err_t ES8311::power_down() {
    if (!m_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Powering down codec");
    write_reg(ES8311_DAC_REG31, 0x00);          // Disable DAC
    write_reg(ES8311_SYSTEM_REG0E, 0x00);       // Power down analog
    write_reg(ES8311_SYSTEM_REG0F, 0x00);       // Power down DAC
    write_reg(ES8311_SYSTEM_REG0D, 0x00);       // Power off
    
    return ESP_OK;
}

esp_err_t ES8311::power_up() {
    if (!m_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Powering up codec");
    write_reg(ES8311_SYSTEM_REG0D, 0x01);       // Power on
    write_reg(ES8311_SYSTEM_REG0E, 0x02);       // Power up analog
    write_reg(ES8311_SYSTEM_REG0F, 0x44);       // Power up DAC
    write_reg(ES8311_DAC_REG31, 0x60);          // Enable DAC, unmute
    
    vTaskDelay(pdMS_TO_TICKS(50));
    set_volume(m_volume);  // Restore volume
    
    return ESP_OK;
}
