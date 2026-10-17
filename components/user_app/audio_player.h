#pragma once

#include <esp_err.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Audio player now backed by audio_bsp (esp_codec_dev)
class AudioPlayer {
public:
    static AudioPlayer& instance();

    // Initialize audio subsystem (audio_bsp + codec)
    esp_err_t init();

    // Deinitialize audio subsystem
    void deinit();

    // Play sound (filename ignored; uses embedded PCM test via audio_bsp)
    esp_err_t play(const char* filename, bool blocking = true);

    // Stop current playback (non-blocking mode)
    void stop();

    // Stop playback and wait until the playback task exits.
    // Timeout in milliseconds; returns immediately if nothing is playing.
    void stop_and_wait(uint32_t timeout_ms = 1000);

    // Check if audio is currently playing
    bool is_playing() const;

    // Set volume (0-100) via audio_bsp
    void set_volume(uint8_t volume);

    // Get current volume (0-100)
    uint8_t get_volume() const;

private:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool initialized_ = false;
    volatile bool playing_ = false;
    volatile bool stop_flag_ = false;
    uint8_t volume_ = 70;
    void* task_handle_ = nullptr; // FreeRTOS TaskHandle_t as void* to avoid header dep

    // Serialize init/deinit/play to avoid I2S races across tasks
    SemaphoreHandle_t mtx_ = nullptr;
    inline void lock() {
        if (!mtx_) {
            mtx_ = xSemaphoreCreateMutex();
        }
        if (mtx_) {
            xSemaphoreTake(mtx_, portMAX_DELAY);
        }
    }
    inline void unlock() {
        if (mtx_) {
            xSemaphoreGive(mtx_);
        }
    }

    // Worker tasks
    static void streamMp3Task(void* arg);
};

#ifdef __cplusplus
extern "C" {
#endif

// C wrapper for button callbacks to play sounds (non-blocking)
void audio_player_play_nonblocking(const char* filename);

#ifdef __cplusplus
}
#endif
