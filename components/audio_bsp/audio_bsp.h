#ifndef AUDIO_BSP_H
#define AUDIO_BSP_H


#ifdef __cplusplus
extern "C" {
#endif

void audio_bsp_init(void);
void audio_playback_set_vol(uint8_t vol);


void audio_play_init(void);

// Re-open playback stream with specific format (e.g., for MP3 sample rate)
void audio_play_open(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);

void audio_playback_read(void *data_ptr,uint32_t len);

void audio_playback_write(void *data_ptr,uint32_t len);

// Optional deinit wrapper
void audio_deinit(void);

// Simple tone generator for bring-up (blocking)
void audio_play_tone(uint32_t freq_hz, uint32_t ms, uint8_t volume_percent);

#ifdef __cplusplus
}
#endif



#endif
