#include "audio_codec.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "audio/codecs/es8374_audio_codec.h"
#include "audio/codecs/es8388_audio_codec.h"
#include "audio/codecs/es8389_audio_codec.h"
#include "audio/codecs/box_audio_codec.h"
#include "audio/codecs/no_audio_codec.h"

#include <new>
#include <esp_log.h>

#define TAG "CodecBridge"

struct codec_wrapper {
    audio_codec_t base;
    AudioCodec *impl;
};

static int codec_wrap_read(audio_codec_t *c, int16_t *dest, int samples) {
    auto *w = reinterpret_cast<codec_wrapper *>(c);
    std::vector<int16_t> buf(samples);
    if (w->impl->InputData(buf)) {
        memcpy(dest, buf.data(), samples * sizeof(int16_t));
        return samples;
    }
    return 0;
}

static int codec_wrap_write(audio_codec_t *c, const int16_t *data, int samples) {
    auto *w = reinterpret_cast<codec_wrapper *>(c);
    std::vector<int16_t> buf(data, data + samples);
    w->impl->OutputData(buf);
    return samples;
}

static void codec_wrap_set_volume(audio_codec_t *c, int volume) {
    auto *w = reinterpret_cast<codec_wrapper *>(c);
    w->impl->SetOutputVolume(volume);
}

static void codec_wrap_set_gain(audio_codec_t *c, float gain) {
    auto *w = reinterpret_cast<codec_wrapper *>(c);
    w->impl->SetInputGain(gain);
}

static void codec_wrap_enable_input(audio_codec_t *c, bool enable) {
    auto *w = reinterpret_cast<codec_wrapper *>(c);
    w->impl->EnableInput(enable);
}

static void codec_wrap_enable_output(audio_codec_t *c, bool enable) {
    auto *w = reinterpret_cast<codec_wrapper *>(c);
    w->impl->EnableOutput(enable);
}

static void codec_wrap_start(audio_codec_t *c) {
    auto *w = reinterpret_cast<codec_wrapper *>(c);
    w->impl->Start();
}

static void codec_wrap_destroy(audio_codec_t *c) {
    auto *w = reinterpret_cast<codec_wrapper *>(c);
    delete w->impl;
    delete w;
}

static const audio_codec_ops_t cpp_codec_ops = {
    .read = codec_wrap_read,
    .write = codec_wrap_write,
    .set_output_volume = codec_wrap_set_volume,
    .set_input_gain = codec_wrap_set_gain,
    .enable_input = codec_wrap_enable_input,
    .enable_output = codec_wrap_enable_output,
    .start = codec_wrap_start,
    .destroy = codec_wrap_destroy,
};

static audio_codec_t *wrap_cpp_codec(AudioCodec *impl) {
    if (!impl) return nullptr;
    auto *w = new (std::nothrow) codec_wrapper{};
    if (!w) { delete impl; return nullptr; }
    w->impl = impl;
    w->base.ops = &cpp_codec_ops;
    w->base.duplex = impl->duplex();
    w->base.input_reference = impl->input_reference();
    w->base.input_sample_rate = impl->input_sample_rate();
    w->base.output_sample_rate = impl->output_sample_rate();
    w->base.input_channels = impl->input_channels();
    w->base.output_channels = impl->output_channels();
    w->base.output_volume = impl->output_volume();
    w->base.input_gain = impl->input_gain();
    w->base.input_enabled = impl->input_enabled();
    w->base.output_enabled = impl->output_enabled();
    return &w->base;
}

extern "C" {

audio_codec_t *es8311_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk, bool pa_inverted)
{
    auto *impl = new (std::nothrow) Es8311AudioCodec(
        i2c_handle, (i2c_port_t)i2c_port, in_rate, out_rate,
        (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws,
        (gpio_num_t)dout, (gpio_num_t)din,
        (gpio_num_t)pa_pin, addr, use_mclk, pa_inverted);
    return wrap_cpp_codec(impl);
}

audio_codec_t *es8374_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk)
{
    auto *impl = new (std::nothrow) Es8374AudioCodec(
        i2c_handle, (i2c_port_t)i2c_port, in_rate, out_rate,
        (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws,
        (gpio_num_t)dout, (gpio_num_t)din,
        (gpio_num_t)pa_pin, addr, use_mclk);
    return wrap_cpp_codec(impl);
}

audio_codec_t *es8388_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool input_reference)
{
    auto *impl = new (std::nothrow) Es8388AudioCodec(
        i2c_handle, (i2c_port_t)i2c_port, in_rate, out_rate,
        (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws,
        (gpio_num_t)dout, (gpio_num_t)din,
        (gpio_num_t)pa_pin, addr, input_reference);
    return wrap_cpp_codec(impl);
}

audio_codec_t *es8389_codec_create(void *i2c_handle, int i2c_port,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t addr, bool use_mclk)
{
    auto *impl = new (std::nothrow) Es8389AudioCodec(
        i2c_handle, (i2c_port_t)i2c_port, in_rate, out_rate,
        (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws,
        (gpio_num_t)dout, (gpio_num_t)din,
        (gpio_num_t)pa_pin, addr, use_mclk);
    return wrap_cpp_codec(impl);
}

audio_codec_t *box_codec_create(void *i2c_handle,
    int in_rate, int out_rate,
    int mclk, int bclk, int ws, int dout, int din,
    int pa_pin, uint8_t es8311_addr, uint8_t es7210_addr,
    bool input_reference)
{
    auto *impl = new (std::nothrow) BoxAudioCodec(
        i2c_handle, in_rate, out_rate,
        (gpio_num_t)mclk, (gpio_num_t)bclk, (gpio_num_t)ws,
        (gpio_num_t)dout, (gpio_num_t)din,
        (gpio_num_t)pa_pin, es8311_addr, es7210_addr, input_reference);
    return wrap_cpp_codec(impl);
}

}
