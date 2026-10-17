#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "adc_bsp.h"
#include "esp_log.h"

static const char *TAG = "adc_bsp";

// Define static members
adc_oneshot_unit_handle_t adc_bsp::s_adc1_handle = NULL;
adc_cali_handle_t adc_bsp::s_cali_handle = NULL;
int adc_bsp::s_ref_count = 0;
std::mutex adc_bsp::s_mutex;

adc_bsp::adc_bsp()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    
    if (s_ref_count == 0) {
        ESP_LOGD(TAG, "Initializing ADC unit (ref_count=0)");
        adc_cali_curve_fitting_config_t cali_config = {};
        cali_config.unit_id = ADC_UNIT_1;
        cali_config.atten = ADC_ATTEN_DB_12;
        cali_config.bitwidth = ADC_BITWIDTH_12; // 4096
        ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali_handle));

        adc_oneshot_unit_init_cfg_t init_config1 = {};
        init_config1.unit_id = ADC_UNIT_1;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &s_adc1_handle));
        
        adc_oneshot_chan_cfg_t config = {};
        config.bitwidth = ADC_BITWIDTH_12;
        config.atten = ADC_ATTEN_DB_12;
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1_handle, ADC_CHANNEL_3, &config));
    } else {
        ESP_LOGD(TAG, "ADC unit already initialized (ref_count=%d)", s_ref_count);
    }
    
    s_ref_count++;
    initialized = true;
}

adc_bsp::~adc_bsp()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    
    if (initialized) {
        s_ref_count--;
        ESP_LOGD(TAG, "Releasing ADC unit (new ref_count=%d)", s_ref_count);
        
        if (s_ref_count == 0) {
            if (s_adc1_handle) {
                ESP_ERROR_CHECK(adc_oneshot_del_unit(s_adc1_handle));
                s_adc1_handle = NULL;
            }
            if (s_cali_handle) {
                ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(s_cali_handle));
                s_cali_handle = NULL;
            }
        }
        initialized = false;
    }
}

float adc_bsp::get_vbatVoltage()
{
    int OriginalData;
    int CalibratedData;
    float VbatVoltage;

    // CRITICAL FIX: ADC operations can interfere with GPIO hold on nearby pins
    // Perform multiple readings and average to reduce transient effects
    const int num_samples = 3;
    int sum = 0;
    for (int i = 0; i < num_samples; i++)
    {
        // Use static handle
        adc_oneshot_read(s_adc1_handle, ADC_CHANNEL_3, &OriginalData);
        sum += OriginalData;
        if (i < num_samples - 1)
        {
            vTaskDelay(pdMS_TO_TICKS(5)); // Small delay between samples
        }
    }
    int avg = sum / num_samples;

    // Use static handle
    adc_cali_raw_to_voltage(s_cali_handle, avg, &CalibratedData);
    VbatVoltage = 0.001 * CalibratedData * 2;

    // Allow ADC subsystem to fully settle after read sequence
    vTaskDelay(pdMS_TO_TICKS(10));

    return VbatVoltage;
}

// Map VBAT (single Li-ion cell) -> % using a discharge curve LUT.
// Numbers are typical @ ~25°C, light load. Calibrate V_FULL/V_EMPTY & LUT if needed.
uint8_t adc_bsp::get_Batterylevel()
{
    float v = get_vbatVoltage(); // e.g., measured at the cell (already divider-corrected)

    // Clamp obvious bounds (choose your own cutoffs if your BMS cuts earlier/later)
    const float V_FULL = 4.20f;  // treat ≥ this as 100%
    const float V_EMPTY = 3.20f; // treat ≤ this as 0% (under load)
    
    // Fix: User reported never reaching 100%. Lower threshold slightly.
    if (v >= 4.15f)
        return 100;
    if (v <= V_EMPTY)
        return 0;

    // Voltage breakpoints (V) and corresponding SoC (%)
    // Adjusted curve based on user feedback:
    // - Faster drop 96-90%
    // - Slower drop 90-80%
    // - Slower drop 80-50%
    // - Slower drop 50-0%
    static const float vLUT[] = {
        4.15f, 4.10f, 4.00f, 3.90f, 3.80f, 
        3.70f, 3.65f, 3.60f, 3.55f, 3.50f, 
        3.45f, 3.40f, 3.35f, 3.30f, 3.25f, 3.20f
    };
    static const uint8_t pLUT[] = {
        100, 95, 90, 85, 80,
        70, 60, 50, 40, 30,
        20, 15, 10, 5, 2, 0
    };
    constexpr int N = sizeof(vLUT) / sizeof(vLUT[0]);

    // Find the segment [hi .. lo] that brackets v (vLUT is descending).
    for (int i = 0; i < N - 1; ++i)
    {
        float v_hi = vLUT[i];
        float v_lo = vLUT[i + 1];
        if (v <= v_hi && v >= v_lo)
        {
            uint8_t p_hi = pLUT[i];
            uint8_t p_lo = pLUT[i + 1];
            // Linear interpolation within the segment.
            float t = (v - v_lo) / (v_hi - v_lo); // 0..1
            float p = p_lo + t * (p_hi - p_lo);
            if (p < 0)
                p = 0;
            if (p > 100)
                p = 100;
            return (uint8_t)(p + 0.5f);
        }
    }
    // Fallback (shouldn't hit due to clamps)
    float p = 100.0f * (v - V_EMPTY) / (V_FULL - V_EMPTY);
    if (p < 0)
        p = 0;
    if (p > 100)
        p = 100;
    return (uint8_t)(p + 0.5f);
}
