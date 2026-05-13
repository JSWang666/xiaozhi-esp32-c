#include "audio_codec.h"
#include "settings.h"

#include <esp_log.h>
#include <string.h>

#define TAG "AudioCodec"

void audio_codec_base_init(audio_codec_t *codec)
{
    if (!codec) return;
    codec->tx_handle = NULL;
    codec->rx_handle = NULL;
    codec->duplex = false;
    codec->input_reference = false;
    codec->input_enabled = false;
    codec->output_enabled = false;
    codec->input_sample_rate = 0;
    codec->output_sample_rate = 0;
    codec->input_channels = 1;
    codec->output_channels = 1;
    codec->output_volume = 70;
    codec->input_gain = 0.0f;
}

void audio_codec_base_start(audio_codec_t *codec)
{
    if (!codec) return;

    settings_t *s = settings_open("audio", false);
    if (s) {
        codec->output_volume = settings_get_int(s, "output_volume", codec->output_volume);
        if (codec->output_volume <= 0) {
            ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", codec->output_volume);
            codec->output_volume = 10;
        }
        settings_close(s);
    }

    if (codec->ops && codec->ops->start)
        codec->ops->start(codec);

    ESP_LOGI(TAG, "Audio codec started");
}

void audio_codec_base_set_output_volume(audio_codec_t *codec, int volume)
{
    if (!codec) return;
    codec->output_volume = volume;
    ESP_LOGI(TAG, "Set output volume to %d", codec->output_volume);

    settings_t *s = settings_open("audio", true);
    if (s) {
        settings_set_int(s, "output_volume", codec->output_volume);
        settings_close(s);
    }
}

void audio_codec_base_set_input_gain(audio_codec_t *codec, float gain)
{
    if (!codec) return;
    codec->input_gain = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", codec->input_gain);
}

void audio_codec_base_enable_input(audio_codec_t *codec, bool enable)
{
    if (!codec) return;
    if (enable == codec->input_enabled) return;
    codec->input_enabled = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void audio_codec_base_enable_output(audio_codec_t *codec, bool enable)
{
    if (!codec) return;
    if (enable == codec->output_enabled) return;
    codec->output_enabled = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}

void audio_codec_output_data(audio_codec_t *codec, int16_t *data, int samples)
{
    if (!codec || !codec->ops || !codec->ops->write) return;
    codec->ops->write(codec, data, samples);
}

bool audio_codec_input_data(audio_codec_t *codec, int16_t *data, int samples)
{
    if (!codec || !codec->ops || !codec->ops->read) return false;
    int got = codec->ops->read(codec, data, samples);
    return got > 0;
}

void audio_codec_destroy(audio_codec_t *codec)
{
    if (!codec) return;
    if (codec->ops && codec->ops->destroy)
        codec->ops->destroy(codec);
}
