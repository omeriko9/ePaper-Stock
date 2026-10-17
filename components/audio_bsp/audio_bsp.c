#include <stdio.h>
#include "audio_bsp.h"
#include "freertos/FreeRTOS.h"
#include "codec_board.h"
#include "codec_init.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <math.h>


#include "driver/i2s_std.h"  // for i2s_chan_handle_t definition

static const char *TAG = "audio_bsp";
esp_codec_dev_handle_t playback = NULL;
esp_codec_dev_handle_t record = NULL;
static bool s_playback_open = false;
static bool s_record_open = false;

// Track I2S TX state
static bool tx_enabled = false;
static i2s_chan_handle_t tx_handle = NULL;

void audio_bsp_set_tx_handle(i2s_chan_handle_t handle)
{
    tx_handle = handle;
    // Since the channel is already enabled in codec_init.c, mark it as enabled here too
    if (tx_handle != NULL) {
        tx_enabled = true;
        ESP_LOGI(TAG, "audio_bsp_set_tx_handle: tx_handle assigned to %p, marked as enabled (was enabled in codec_init)", tx_handle);
    } else {
        tx_enabled = false;
        ESP_LOGI(TAG, "audio_bsp_set_tx_handle: tx_handle set to NULL, marked as disabled");
    }
}


void audio_bsp_init(void)
{
	codec_init_cfg_t codec_dev = {0};
	codec_dev.out_mode = CODEC_I2S_MODE_STD;   // force standard I2S for ES8311
	codec_dev.in_mode  = CODEC_I2S_MODE_STD;   // standard RX path
	codec_dev.in_use_tdm = false;
	codec_dev.reuse_dev  = true;

	ESP_LOGI(TAG, "audio_bsp_init: starting audio BSP initialization");
	ESP_LOGI(TAG, "audio_bsp_init: tx_handle=%p, tx_enabled=%d", tx_handle, tx_enabled);

	set_codec_board_type("S3_ePaper_1_54");
	init_codec(&codec_dev);
    playback = get_playback_handle();
    record = get_record_handle();

	ESP_LOGI(TAG, "audio_bsp_init: codec initialized, playback=%p, record=%p", playback, record);

	// Ensure external PA (GPIO46) is enabled (active-HIGH on this board)
	gpio_reset_pin(GPIO_NUM_46);
	gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT);
	gpio_set_level(GPIO_NUM_46, 1);
    ESP_LOGI(TAG, "PA GPIO46 set HIGH");
}

void audio_playback_set_vol(uint8_t vol)
{
  	esp_codec_dev_set_out_vol(playback, vol); //设置60声音大小
}

void audio_play_init(void)
{
    esp_codec_dev_set_out_vol(playback, 100.0);
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = 48000;
    fs.channel = 2;
    fs.bits_per_sample = 16;

    ESP_LOGI(TAG, "audio_play_init: playback=%p record=%p tx_handle=%p tx_enabled=%d", playback, record, tx_handle, tx_enabled);

    if (tx_handle == NULL) {
        ESP_LOGE(TAG, "audio_play_init: cannot enable TX — tx_handle is NULL");
        return;
    }

    // Safely toggle I2S-channel enablement sequence
    ESP_LOGI(TAG, "audio_play_init: checking if I2S TX needs to be enabled (tx_enabled=%d)", tx_enabled);
    if (!tx_enabled) {
        esp_err_t en_res = i2s_channel_enable(tx_handle);
        ESP_LOGI(TAG, "audio_play_init: i2s_channel_enable() called, result=%d (%s)", en_res, esp_err_to_name(en_res));
        if (en_res == ESP_OK || en_res == ESP_ERR_INVALID_STATE) {
            // INVALID_STATE means already enabled; treat as enabled
            tx_enabled = true;
            ESP_LOGI(TAG, "audio_play_init: I2S TX channel enabled (res=%d)", en_res);
        } else {
            ESP_LOGE(TAG, "audio_play_init: failed to enable I2S TX channel (err=%d)", en_res);
        }
    } else {
        ESP_LOGW(TAG, "audio_play_init: I2S TX channel already marked enabled - skipping re-enable");
    }

    if (playback) {
        esp_err_t open_res = esp_codec_dev_open(playback, &fs);
        ESP_LOGI(TAG, "audio_play_init: esp_codec_dev_open(playback) result=%d", open_res);
        if (open_res == ESP_CODEC_DEV_OK) {
            s_playback_open = true;
            if (tx_handle == NULL) {
                ESP_LOGE(TAG, "audio_play_init: tx_handle is NULL before enabling I2S!");
            } else if (!tx_enabled) {
                esp_err_t tx_res = i2s_channel_enable(tx_handle);
                ESP_LOGI(TAG, "audio_play_init: i2s_channel_enable result=%d handle=%p", tx_res, tx_handle);
                if (tx_res == ESP_OK || tx_res == ESP_ERR_INVALID_STATE) {
                    tx_enabled = true;
                }
            }
        } else {
            ESP_LOGE(TAG, "audio_play_init: playback open failed!");
            s_playback_open = false;
        }
    } else {
        ESP_LOGE(TAG, "audio_play_init: playback handle is NULL!");
    }

    if (record) {
        esp_err_t rec_res = esp_codec_dev_open(record, &fs);
        ESP_LOGI(TAG, "audio_play_init: esp_codec_dev_open(record) result=%d", rec_res);
        if (rec_res == ESP_CODEC_DEV_OK) {
            s_record_open = true;
            esp_codec_dev_set_in_gain(record, 15.0f);
        } else {
            ESP_LOGE(TAG, "audio_play_init: record open failed!");
        }
    } else {
        ESP_LOGW(TAG, "audio_play_init: record handle is NULL");
    }

    esp_codec_dev_set_out_mute(playback, false);
    ESP_LOGI(TAG, "audio_play_init: playback opened: %lu Hz, ch=%d, bits=%d", (unsigned long)fs.sample_rate, fs.channel, fs.bits_per_sample);

    uint8_t zero[256] = {0};
    for (int i = 0; i < 12; ++i) {
        esp_codec_dev_write(playback, zero, sizeof(zero));
    }
}

void audio_play_open(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample)
{
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = sample_rate;
    fs.channel = channels;
    fs.bits_per_sample = bits_per_sample;
    
    ESP_LOGI(TAG, "audio_play_open: reopening with %lu Hz, %d ch, %d bits. Current state: playback_open=%d, record_open=%d, tx_enabled=%d", 
             (unsigned long)sample_rate, channels, bits_per_sample, s_playback_open, s_record_open, tx_enabled);
    
    // Re-open playback with new format
    if (playback && s_playback_open) {
        ESP_LOGI(TAG, "audio_play_open: closing playback codec");
        esp_codec_dev_close(playback);
        s_playback_open = false;
    }
    if (record && s_record_open) {
        ESP_LOGI(TAG, "audio_play_open: closing record codec");
        esp_codec_dev_close(record);
        s_record_open = false;
    }
    
    // Since we closed both codec devices, the I2S channel might have been disabled internally
    // Mark it as disabled so it gets re-enabled when we reopen
    if (tx_enabled) {
        ESP_LOGI(TAG, "audio_play_open: marking I2S TX as disabled after closing codecs");
        tx_enabled = false;
    }
    
    if (playback && esp_codec_dev_open(playback, &fs) == ESP_CODEC_DEV_OK) {
        ESP_LOGI(TAG, "playback re-opened: %lu Hz, ch=%d, bits=%d", (unsigned long)fs.sample_rate, fs.channel, fs.bits_per_sample);
        s_playback_open = true;
        if (!tx_enabled) {
            esp_err_t tx_res = i2s_channel_enable(tx_handle);
            ESP_LOGI(TAG, "audio_play_open: i2s_channel_enable result=%d, tx_handle=%p, tx_enabled was %d", tx_res, tx_handle, tx_enabled);
            if (tx_res == ESP_OK || tx_res == ESP_ERR_INVALID_STATE) {
                tx_enabled = true;
                ESP_LOGI(TAG, "I2S TX channel enabled (audio_play_open)");
            } else {
                ESP_LOGW(TAG, "I2S TX enable failed in audio_play_open (err=%d)", tx_res);
            }
        }
        if (record) {
            if (esp_codec_dev_open(record, &fs) == ESP_CODEC_DEV_OK) {
                s_record_open = true;
                esp_codec_dev_set_in_gain(record, 15.0f);
            }
        }
        esp_codec_dev_set_out_mute(playback, false);
        // Prime after reopen
        uint8_t zero[256] = {0};
        for (int i = 0; i < 12; ++i) {
            esp_codec_dev_write(playback, zero, sizeof(zero));
        }
    } else {
        ESP_LOGE(TAG, "playback open failed: %lu Hz, ch=%d, bits=%d", (unsigned long)fs.sample_rate, fs.channel, fs.bits_per_sample);
    }
}

void audio_playback_read(void *data_ptr,uint32_t len)
{
	esp_codec_dev_read(record, data_ptr, len);
}

void audio_playback_write(void *data_ptr,uint32_t len)
{
    if (playback == NULL || !s_playback_open) {
        return; // playback not initialized or already deinitialized
    }
    uint8_t *p = (uint8_t *)data_ptr;
    uint32_t remain = len;
    const uint32_t CHUNK = 256;
    int idle = 0;
    while (remain > 0) {
        uint32_t n = remain > CHUNK ? CHUNK : remain;
        int ret = esp_codec_dev_write(playback, p, n);
        if (ret != ESP_CODEC_DEV_OK) {
            // Let DMA catch up
            if ((++idle % 16) == 0) {
                ESP_LOGW(TAG, "write not ready (ret=%d) for %lu bytes (remain=%lu)", ret, (unsigned long)n, (unsigned long)remain);
            }
            vTaskDelay(1);
            continue;
        }
        p += n;
        remain -= n;
        idle = 0;
    }
}

void audio_deinit(void)
{
    ESP_LOGI(TAG, "audio_deinit: starting deinit, tx_handle=%p, tx_enabled=%d", tx_handle, tx_enabled);
    
    if (tx_enabled && tx_handle) {
        ESP_LOGI(TAG, "audio_deinit: attempting to disable I2S TX channel");
        esp_err_t dis_res = i2s_channel_disable(tx_handle);
        ESP_LOGI(TAG, "audio_deinit: i2s_channel_disable result=%d (%s)", dis_res, esp_err_to_name(dis_res));
        if (dis_res == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "audio_deinit: I2S TX channel was already disabled (INVALID_STATE)");
        } else if (dis_res != ESP_OK) {
            ESP_LOGW(TAG, "audio_deinit: I2S TX disable returned %d", dis_res);
        } else {
            ESP_LOGI(TAG, "audio_deinit: I2S TX channel disabled successfully");
        }
        tx_enabled = false;
    } else {
        ESP_LOGI(TAG, "audio_deinit: I2S TX channel already disabled or handle is NULL");
    }
    
    if (playback && s_playback_open) {
        ESP_LOGI(TAG, "audio_deinit: closing playback codec");
        esp_codec_dev_close(playback);
        s_playback_open = false;
    }
    if (record && s_record_open) {
        ESP_LOGI(TAG, "audio_deinit: closing record codec");
        esp_codec_dev_close(record);
        s_record_open = false;
    }
    
    ESP_LOGI(TAG, "audio_deinit: calling deinit_codec()");
    deinit_codec();
    playback = NULL;
    record = NULL;
    ESP_LOGI(TAG, "audio_deinit: completed");
}

void audio_play_tone(uint32_t freq_hz, uint32_t ms, uint8_t volume_percent)
{
    if (playback == NULL) {
        return;
    }
    // Ensure playback is open at 16k for predictable tone period
    audio_play_open(16000, 2, 16);
    if (volume_percent > 100) volume_percent = 100;
    float vol = (float)volume_percent;
    esp_codec_dev_set_out_vol(playback, vol);

    const uint32_t sample_rate = 16000;
    const uint32_t total_samples = (ms * sample_rate) / 1000;
    const float two_pi = 6.283185307179586f;
    const float phase_inc = two_pi * (float)freq_hz / (float)sample_rate;
    int16_t buf[256 * 2]; // stereo interleaved
    float phase = 0.0f;
    uint32_t remaining = total_samples;
    // amplitude scaled to ~50% to avoid clipping
    const float amp = 0.5f * 32767.0f;
    while (remaining > 0) {
        uint32_t n = remaining > 256 ? 256 : remaining;
        for (uint32_t i = 0; i < n; ++i) {
            int16_t s = (int16_t)(sinf(phase) * amp);
            phase += phase_inc;
            if (phase > two_pi) phase -= two_pi;
            buf[2 * i + 0] = s;
            buf[2 * i + 1] = s;
        }
        esp_codec_dev_write(playback, (uint8_t *)buf, n * 2 /*ch*/ * sizeof(int16_t));
        remaining -= n;
    }
}
