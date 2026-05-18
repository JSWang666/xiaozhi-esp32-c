#ifndef _DUMMY_AUDIO_CODEC_H
#define _DUMMY_AUDIO_CODEC_H

#include "audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

audio_codec_t *dummy_audio_codec_create(int input_sample_rate, int output_sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* _DUMMY_AUDIO_CODEC_H */
