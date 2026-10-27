/*
 * CoreS3 audio out: the AW88298 amplifier (I2C 0x36) fed by I2S, both set
 * up by the CoreS3 BSP with esp_codec_dev. There is no headphone jack, so
 * everything goes to the speaker; its rail is only up while playing.
 */
#include "aout.h"

#include "bsp/m5stack_core_s3.h"
#include "deck_hal.h"
#include "esp_codec_dev.h"
#include "esp_log.h"

static const char *TAG = "aout";

static esp_codec_dev_handle_t s_dev;
static bool s_open;
static int s_volume = 60;

static bool setup(void)
{
    if (s_dev) return true;
    if (bsp_audio_init(NULL) != ESP_OK) return false;
    s_dev = bsp_audio_codec_speaker_init();
    return s_dev != NULL;
}

bool aout_open(uint32_t rate, int channels)
{
    hal_speaker_amp(true);
    if (!setup()) {
        ESP_LOGE(TAG, "codec setup failed");
        return false;
    }
    if (s_open) esp_codec_dev_close(s_dev);
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = (uint8_t)channels, .sample_rate = rate};
    s_open = esp_codec_dev_open(s_dev, &fs) == ESP_CODEC_DEV_OK;
    if (!s_open) {
        ESP_LOGE(TAG, "codec open %lu Hz x%d failed", (unsigned long)rate, channels);
        return false;
    }
    esp_codec_dev_set_out_vol(s_dev, s_volume);
    ESP_LOGI(TAG, "%lu Hz x%d to speaker", (unsigned long)rate, channels);
    return true;
}

void aout_write(const int16_t *pcm, size_t count)
{
    if (!s_open) return;
    esp_codec_dev_write(s_dev, (void *)pcm, (int)(count * sizeof(int16_t))); /* bytes */
}

void aout_close(void)
{
    if (s_open) esp_codec_dev_close(s_dev);
    s_open = false;
    hal_speaker_amp(false);
}

void aout_volume(int percent)
{
    s_volume = percent;
    if (s_open) esp_codec_dev_set_out_vol(s_dev, percent);
}

void aout_route(bool headphones)
{
    (void)headphones; /* speaker only */
}
