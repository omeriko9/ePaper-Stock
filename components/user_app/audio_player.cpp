#include "audio_player.h"
#include "settings.h"
#include "audio_bsp.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_spiffs.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char *TAG = "audio";

extern "C"
{
// Helix MP3 decoder
#include "mp3dec.h"
}

AudioPlayer &AudioPlayer::instance()
{
    static AudioPlayer player;
    return player;
}

AudioPlayer::AudioPlayer() {}

AudioPlayer::~AudioPlayer()
{
    deinit();
}

esp_err_t AudioPlayer::init()
{
    lock();
    if (initialized_) {
        unlock();
        return ESP_OK;
    }

    // The audio rail is already on (power_mgmt keeps it on, also in deep sleep).
    // Initialize BSP and codec (board-specific setup handled inside)
    ESP_LOGI(TAG, "Initializing audio_bsp...");
    audio_bsp_init();
    audio_play_init(); // 16kHz / 16-bit / stereo

    // Apply saved volume
    volume_ = Settings::instance().audio_volume();
    audio_playback_set_vol(volume_);

    // Mount /audio SPIFFS partition for optional MP3 files
    esp_vfs_spiffs_conf_t audio_conf = {
        .base_path = "/audio",
        .partition_label = "audio",
        .max_files = 3,
        .format_if_mount_failed = false};
    esp_err_t mret = esp_vfs_spiffs_register(&audio_conf);
    if (mret != ESP_OK)
    {
        ESP_LOGW(TAG, "Audio SPIFFS mount failed: %s (embedded PCM fallback only)", esp_err_to_name(mret));
    }
    else
    {
        ESP_LOGI(TAG, "Audio SPIFFS mounted at /audio");
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Audio initialized (audio_bsp)");
    unlock();
    return ESP_OK;
}

void AudioPlayer::deinit()
{
    lock();
    if (!initialized_) {
        unlock();
        return;
    }

    // Ensure streaming task is asked to stop and has time to exit
    stop_and_wait(1500);

    // Deinitialize codec (optional)
    audio_deinit();

    initialized_ = false;
    ESP_LOGI(TAG, "Audio player deinitialized");
    unlock();
}

// no embedded PCM path; we play from /audio only

typedef struct
{
    AudioPlayer *self;
    char path[128];
} mp3_task_arg_t;

void AudioPlayer::streamMp3Task(void *a)
{
    mp3_task_arg_t *parg = (mp3_task_arg_t *)a;
    AudioPlayer *self = parg->self;
    char path[128];
    std::snprintf(path, sizeof(path), "%s", parg->path);
    free(parg);

    ESP_LOGI(TAG, "Opening MP3: %s", path);
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "MP3 open failed: %s", path);
        self->playing_ = false;
        vTaskDelete(nullptr);
        return;
    }

    // Get file size with task yields to avoid blocking other tasks on SPIFFS I/O
    long fsz = 0; 
    {
        fseek(f, 0, SEEK_END);
        taskYIELD();  // Allow other tasks to run
        fsz = ftell(f);
        taskYIELD();  // Allow other tasks to run
        fseek(f, 0, SEEK_SET);
        taskYIELD();  // Allow other tasks to run
    }
    ESP_LOGI(TAG, "MP3 size: %ld bytes", fsz);

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec)
    {
        fclose(f);
        self->playing_ = false;
        vTaskDelete(nullptr);
        return;
    }

    const int MP3_BUF = 4096;
    uint8_t *mp3 = (uint8_t *)malloc(MP3_BUF);
    int16_t *pcm = (int16_t *)malloc(4608); // enough for typical 1152 samples stereo
    int16_t *stereo = nullptr;
    size_t stereo_cap = 0;
    int bytes_left = 0;
    size_t total_written = 0;
    bool format_set = false;
    int frames = 0;
    int errors = 0;
    while (!self->stop_flag_)
    {
        if (bytes_left < 1024)
        {
            size_t r = fread(mp3 + bytes_left, 1, MP3_BUF - (size_t)bytes_left, f);
            if (r == 0 && bytes_left == 0)
                break;
            bytes_left += (int)r;
            taskYIELD();  // Yield after file I/O to allow other tasks
        }
        uint8_t *read_ptr = mp3;
        int offset = MP3FindSyncWord(read_ptr, (int)bytes_left);
        if (offset < 0)
        {
            // need more
            if (bytes_left > 0)
                memmove(mp3, read_ptr, (size_t)bytes_left);
            continue;
        }
        read_ptr += offset;
        bytes_left -= offset;
        int err = MP3Decode(dec, &read_ptr, &bytes_left, pcm, 0);
        if (err != ERR_MP3_NONE)
        {
            // skip a byte
            if (bytes_left > 0)
            {
                read_ptr++;
                bytes_left--;
            }
            if (read_ptr != mp3 && bytes_left > 0)
                memmove(mp3, read_ptr, (size_t)bytes_left);
            errors++;
            continue;
        }
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (!format_set)
        {
            // Open codec with detected format only if different than default 48k
            if (fi.samprate != 48000) {
                audio_play_open(fi.samprate, fi.nChans == 2 ? 2 : 2, 16);
            }
            ESP_LOGI(TAG, "MP3 format: %d Hz, ch=%d, outSamps=%d", fi.samprate, fi.nChans, fi.outputSamps);
            format_set = true;
        }
        int samples = fi.outputSamps;
        if (fi.nChans == 1)
        {
            size_t need = samples * 2;
            if (stereo_cap < need)
            {
                free(stereo);
                stereo = (int16_t *)malloc(need * sizeof(int16_t));
                stereo_cap = stereo ? need : 0;
            }
            if (!stereo)
                break;
            for (int i = 0, j = 0; i < samples; ++i)
            {
                int16_t s = pcm[i];
                stereo[j++] = s;
                stereo[j++] = s;
            }
            size_t bytes = need * sizeof(int16_t);
            audio_playback_write(stereo, bytes);
            total_written += bytes;
        }
        else
        {
            size_t bytes = (size_t)samples * sizeof(int16_t);
            audio_playback_write(pcm, bytes);
            total_written += bytes;
        }
        if (bytes_left > 0 && read_ptr != mp3)
            memmove(mp3, read_ptr, (size_t)bytes_left);
        frames++;
        if ((frames % 16) == 0)
        {
            //ESP_LOGI(TAG, "Decoded %d frames, wrote %u bytes", frames, (unsigned)total_written);
        }
    }
    MP3FreeDecoder(dec);
    free(mp3);
    free(pcm);
    free(stereo);
    fclose(f);
    ESP_LOGI(TAG, "MP3 playback done: frames=%d, errors=%d, total_bytes=%u", frames, errors, (unsigned)total_written);
    self->playing_ = false;
    self->task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t AudioPlayer::play(const char *filename, bool blocking)
{
    // Do not power up the codec for a sound we will not play.
    if (!Settings::instance().sound_enabled())
        return ESP_OK;

    // Serialize init/play against post-wake init/deinit
    lock();
    if (!initialized_) {
        esp_err_t ir = init();
        if (ir != ESP_OK) {
            unlock();
            return ir;
        }
    }

    // Stop previous playback if any
    if (playing_)
        stop();

    stop_flag_ = false;
    playing_ = true;

    // Try file from /audio if provided
    char path[128] = {0};
    bool have_file = false;
    if (filename && *filename)
    {
        snprintf(path, sizeof(path), "/audio/%s", filename);
        FILE *f = fopen(path, "rb");
        if (f)
        {
            have_file = true;
            fclose(f);
        }
    }

    if (have_file && !blocking)
    {
        // Start MP3 streaming task
        mp3_task_arg_t *a = (mp3_task_arg_t *)malloc(sizeof(mp3_task_arg_t));
        a->self = this;
        strncpy(a->path, path, sizeof(a->path));
        a->path[sizeof(a->path) - 1] = '\0';
        BaseType_t ok = xTaskCreate(AudioPlayer::streamMp3Task, "mp3_stream", 6144, a, tskIDLE_PRIORITY + 1, reinterpret_cast<TaskHandle_t *>(&task_handle_));
        if (ok != pdPASS)
        {
            playing_ = false;
            free(a);
            unlock();
            return ESP_FAIL;
        }
        unlock();
        return ESP_OK;
    }

    if (have_file && blocking)
    {
        // Blocking decode inline (no task deletion)
        FILE *f = fopen(path, "rb");
        if (!f)
        {
            playing_ = false;
            unlock();
            return ESP_ERR_NOT_FOUND;
        }
        HMP3Decoder dec = MP3InitDecoder();
        if (!dec)
        {
            fclose(f);
            playing_ = false;
            unlock();
            return ESP_FAIL;
        }
        const int MP3_BUF = 4096;
        uint8_t *mp3 = (uint8_t *)malloc(MP3_BUF);
        int16_t *pcm = (int16_t *)malloc(4608);
        int16_t *stereo = nullptr;
        size_t stereo_cap = 0;
        int bytes_left = 0;
        bool format_set = false;
        while (!stop_flag_)
        {
            if (bytes_left < 1024)
            {
                size_t r = fread(mp3 + bytes_left, 1, MP3_BUF - (size_t)bytes_left, f);
                if (r == 0 && bytes_left == 0)
                    break;
                bytes_left += (int)r;
            }
            uint8_t *read_ptr = mp3;
            int offset = MP3FindSyncWord(read_ptr, bytes_left);
            if (offset < 0)
            {
                if (bytes_left > 0)
                    memmove(mp3, read_ptr, (size_t)bytes_left);
                continue;
            }
            read_ptr += offset;
            bytes_left -= offset;
            int err = MP3Decode(dec, &read_ptr, &bytes_left, pcm, 0);
            if (err != ERR_MP3_NONE)
            {
                if (bytes_left > 0)
                {
                    read_ptr++;
                    bytes_left--;
                }
                if (read_ptr != mp3 && bytes_left > 0)
                    memmove(mp3, read_ptr, (size_t)bytes_left);
                continue;
            }
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);
            if (!format_set)
            {
                audio_play_open(fi.samprate, fi.nChans == 2 ? 2 : 2, 16);
                format_set = true;
            }
            int samples = fi.outputSamps;
            if (fi.nChans == 1)
            {
                size_t need = (size_t)samples * 2;
                if (stereo_cap < need)
                {
                    free(stereo);
                    stereo = (int16_t *)malloc(need * sizeof(int16_t));
                    stereo_cap = stereo ? need : 0;
                }
                if (!stereo)
                    break;
                for (int i = 0, j = 0; i < samples; ++i)
                {
                    int16_t s = pcm[i];
                    stereo[j++] = s;
                    stereo[j++] = s;
                }
                audio_playback_write(stereo, need * sizeof(int16_t));
            }
            else
            {
                audio_playback_write(pcm, (size_t)samples * sizeof(int16_t));
            }
            if (bytes_left > 0 && read_ptr != mp3)
                memmove(mp3, read_ptr, (size_t)bytes_left);
        }
        MP3FreeDecoder(dec);
        free(mp3);
        free(pcm);
        free(stereo);
        fclose(f);
        playing_ = false;
        unlock();
        return ESP_OK;
    }
    // No file found; nothing to play
    ESP_LOGW(TAG, "Audio file not found: %s", filename ? filename : "(null)");
    playing_ = false;
    unlock();
    return ESP_ERR_NOT_FOUND;
}

void AudioPlayer::stop()
{
    stop_flag_ = true;
}

void AudioPlayer::stop_and_wait(uint32_t timeout_ms)
{
    // Signal stop and wait for playback task to exit
    stop_flag_ = true;
    TickType_t start = xTaskGetTickCount();
    while (playing_) {
        // Yield to let the streaming task observe stop_flag_
        vTaskDelay(pdMS_TO_TICKS(10));
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            break;
        }
    }
}

void AudioPlayer::set_volume(uint8_t volume)
{
    if (volume > 100)
    {
        volume = 100;
    }
    volume_ = volume;
    audio_playback_set_vol(volume);
    ESP_LOGI(TAG, "Volume set to %d%% (audio_bsp)", volume);
}

uint8_t AudioPlayer::get_volume() const
{
    return volume_;
}

bool AudioPlayer::is_playing() const
{
    return playing_;
}

// void AudioPlayer::set_volume(uint8_t volume) {
//     if (volume > 100) volume = 100;
//     volume_ = volume;
//     ESP_LOGI(TAG, "Volume set to %d%%", volume_);
// }

// C wrapper for button callbacks
extern "C" void audio_player_play_nonblocking(const char* filename)
{
    if (filename && *filename) {
        AudioPlayer::instance().play(filename, false);
    }
}
