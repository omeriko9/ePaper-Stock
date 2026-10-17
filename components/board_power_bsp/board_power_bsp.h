#ifndef BOARD_POWER_BSP_H
#define BOARD_POWER_BSP_H

class board_power_bsp_t
{
private:
    const uint8_t epd_power_pin;
    const uint8_t audio_power_pin;
    const uint8_t vbat_power_pin;


public:
    board_power_bsp_t(uint8_t _epd_power_pin,uint8_t _audio_power_pin,uint8_t _vbat_power_pin, bool should_enable_audio = true);
    ~board_power_bsp_t();

    void POWER_EPD_ON();
    void POWER_EPD_OFF();
    void POWER_Audio_ON();
    void POWER_Audio_OFF();
};

#endif
