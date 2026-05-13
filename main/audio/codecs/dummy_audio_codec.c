#include "audio_codec.h"
#include <stdlib.h>

typedef struct {
    audio_codec_t base;
} dummy_audio_codec_t;

static int dummy_read(audio_codec_t *codec, int16_t *dest, int samples) {
    (void)codec; (void)dest; (void)samples;
    return 0;
}

static int dummy_write(audio_codec_t *codec, const int16_t *data, int samples) {
    (void)codec; (void)data; (void)samples;
    return 0;
}

static void dummy_destroy(audio_codec_t *codec) {
    free(codec);
}

static const audio_codec_ops_t dummy_ops = {
    .read = dummy_read,
    .write = dummy_write,
    .set_output_volume = audio_codec_base_set_output_volume,
    .set_input_gain = audio_codec_base_set_input_gain,
    .enable_input = audio_codec_base_enable_input,
    .enable_output = audio_codec_base_enable_output,
    .start = audio_codec_base_start,
    .destroy = dummy_destroy,
};

audio_codec_t *dummy_audio_codec_create(int input_sample_rate, int output_sample_rate) {
    dummy_audio_codec_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    audio_codec_base_init(&d->base);
    d->base.ops = &dummy_ops;
    d->base.duplex = true;
    d->base.input_reference = false;
    d->base.input_channels = 1;
    d->base.input_sample_rate = input_sample_rate;
    d->base.output_sample_rate = output_sample_rate;

    return &d->base;
}
