#include "audio_c_api.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio_service.h"

struct audio_service {
    AudioService impl;
    audio_callbacks_t callbacks;
    std::string last_wake_word;
};

static audio_service_t *s_audio_instance = nullptr;

audio_service_t *audio_service_get_instance(void) {
    return s_audio_instance;
}

static AudioServiceCallbacks to_cpp_callbacks(audio_service_t* svc) {
    AudioServiceCallbacks cb = {};
    cb.on_send_queue_available = [svc]() {
        if (svc->callbacks.on_send_queue_available) {
            svc->callbacks.on_send_queue_available(svc->callbacks.user_ctx);
        }
    };
    cb.on_wake_word_detected = [svc](const std::string& wake_word) {
        svc->last_wake_word = wake_word;
        if (svc->callbacks.on_wake_word) {
            svc->callbacks.on_wake_word(svc->callbacks.user_ctx, wake_word.c_str());
        }
    };
    cb.on_vad_change = [svc](bool speaking) {
        if (svc->callbacks.on_vad_change) {
            svc->callbacks.on_vad_change(svc->callbacks.user_ctx, speaking);
        }
    };
    return cb;
}

audio_service_t *audio_service_create(const audio_service_cfg_t *cfg) {
    (void)cfg;
    auto *svc = new (std::nothrow) audio_service_t{};
    if (svc) s_audio_instance = svc;
    return svc;
}

void audio_service_destroy(audio_service_t *svc) {
    delete svc;
}

esp_err_t audio_service_init(audio_service_t *svc, void *audio_codec_handle) {
    if (svc == nullptr || audio_codec_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.Initialize(static_cast<AudioCodec*>(audio_codec_handle));
    return ESP_OK;
}

esp_err_t audio_service_start(audio_service_t *svc) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.Start();
    return ESP_OK;
}

esp_err_t audio_service_stop(audio_service_t *svc) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.Stop();
    return ESP_OK;
}

esp_err_t audio_service_set_callbacks(audio_service_t *svc, const audio_callbacks_t *callbacks) {
    if (svc == nullptr || callbacks == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->callbacks = *callbacks;
    AudioServiceCallbacks cpp_cb = to_cpp_callbacks(svc);
    svc->impl.SetCallbacks(cpp_cb);
    return ESP_OK;
}

esp_err_t audio_service_enable_wake_word(audio_service_t *svc, bool enable) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.EnableWakeWordDetection(enable);
    return ESP_OK;
}

esp_err_t audio_service_enable_voice_processing(audio_service_t *svc, bool enable) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.EnableVoiceProcessing(enable);
    return ESP_OK;
}

esp_err_t audio_service_enable_audio_testing(audio_service_t *svc, bool enable) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.EnableAudioTesting(enable);
    return ESP_OK;
}

bool audio_service_is_idle(audio_service_t *svc) {
    return svc && svc->impl.IsIdle();
}

bool audio_service_is_voice_detected(const audio_service_t *svc) {
    return svc && svc->impl.IsVoiceDetected();
}

const char *audio_service_get_last_wake_word(audio_service_t *svc) {
    if (svc == nullptr) {
        return "";
    }
    return svc->last_wake_word.c_str();
}

esp_err_t audio_service_push_decode_packet(audio_service_t *svc, const audio_packet_t *packet) {
    if (svc == nullptr || packet == nullptr || packet->payload == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto cpp_packet = std::make_unique<AudioStreamPacket>();
    cpp_packet->sample_rate = packet->sample_rate;
    cpp_packet->frame_duration = packet->frame_duration_ms;
    cpp_packet->timestamp = packet->timestamp;
    cpp_packet->payload.assign(packet->payload, packet->payload + packet->payload_size);
    return svc->impl.PushPacketToDecodeQueue(std::move(cpp_packet), false) ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_service_pop_send_packet(audio_service_t *svc, audio_packet_t *out_packet) {
    if (svc == nullptr || out_packet == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto packet = svc->impl.PopPacketFromSendQueue();
    if (!packet) {
        return ESP_ERR_NOT_FOUND;
    }
    out_packet->sample_rate = packet->sample_rate;
    out_packet->frame_duration_ms = packet->frame_duration;
    out_packet->timestamp = packet->timestamp;
    out_packet->payload_size = packet->payload.size();
    out_packet->payload = static_cast<uint8_t*>(malloc(out_packet->payload_size));
    if (out_packet->payload == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_packet->payload, packet->payload.data(), out_packet->payload_size);
    return ESP_OK;
}

esp_err_t audio_service_pop_wake_word_packet(audio_service_t *svc, audio_packet_t *out_packet) {
    if (svc == nullptr || out_packet == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto packet = svc->impl.PopWakeWordPacket();
    if (!packet) {
        return ESP_ERR_NOT_FOUND;
    }
    out_packet->sample_rate = packet->sample_rate;
    out_packet->frame_duration_ms = packet->frame_duration;
    out_packet->timestamp = packet->timestamp;
    out_packet->payload_size = packet->payload.size();
    out_packet->payload = static_cast<uint8_t*>(malloc(out_packet->payload_size));
    if (out_packet->payload == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_packet->payload, packet->payload.data(), out_packet->payload_size);
    return ESP_OK;
}

void audio_service_free_packet(audio_packet_t *packet) {
    if (packet == nullptr) {
        return;
    }
    free(packet->payload);
    packet->payload = nullptr;
    packet->payload_size = 0;
}

esp_err_t audio_service_play_sound(audio_service_t *svc, const char *sound_name) {
    if (svc == nullptr || sound_name == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.PlaySound(sound_name);
    return ESP_OK;
}

esp_err_t audio_service_reset_decoder(audio_service_t *svc) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.ResetDecoder();
    return ESP_OK;
}

esp_err_t audio_service_encode_wake_word(audio_service_t *svc) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.EncodeWakeWord();
    return ESP_OK;
}

esp_err_t audio_service_enable_device_aec(audio_service_t *svc, bool enable) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.EnableDeviceAec(enable);
    return ESP_OK;
}

void audio_service_wait_for_playback_empty(audio_service_t *svc) {
    if (svc) {
        svc->impl.WaitForPlaybackQueueEmpty();
    }
}

bool audio_service_is_wake_word_running(const audio_service_t *svc) {
    return svc && svc->impl.IsWakeWordRunning();
}

bool audio_service_is_audio_processor_running(const audio_service_t *svc) {
    return svc && svc->impl.IsAudioProcessorRunning();
}

bool audio_service_is_afe_wake_word(audio_service_t *svc) {
    return svc && svc->impl.IsAfeWakeWord();
}

esp_err_t audio_service_set_models_list(audio_service_t *svc, void *models) {
    if (svc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    svc->impl.SetModelsList(static_cast<srmodel_list_t*>(models));
    return ESP_OK;
}
