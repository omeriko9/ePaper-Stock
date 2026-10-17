#ifndef ADC_BSP_H
#define ADC_BSP_H

#include "esp_adc/adc_oneshot.h"
#include <mutex>

class adc_bsp
{
private:   
    // Static members for shared resource management
    static adc_oneshot_unit_handle_t s_adc1_handle;
    static adc_cali_handle_t s_cali_handle;
    static int s_ref_count;
    static std::mutex s_mutex;

    bool initialized = false;

public:
    adc_bsp();
    ~adc_bsp();

    float get_vbatVoltage();
    uint8_t get_Batterylevel();
};

#endif
