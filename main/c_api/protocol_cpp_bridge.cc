#include "protocol_c_api.h"

#include <memory>
#include <string>

#include "protocol.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "cJSON.h"

struct protocol_handle {
    std::unique_ptr<Protocol> impl;
    protocol_callbacks_t callbacks;
};

static void wire_callbacks(protocol_handle_t* p) {
    p->impl->OnConnected([p]() {
        if (p->callbacks.on_connected) {
            p->callbacks.on_connected(p->callbacks.user_ctx);
        }
    });
    p->impl->OnDisconnected([p]() {
        if (p->callbacks.on_disconnected) {
            p->callbacks.on_disconnected(p->callbacks.user_ctx);
        }
    });
    p->impl->OnNetworkError([p](const std::string& message) {
        if (p->callbacks.on_network_error) {
            p->callbacks.on_network_error(p->callbacks.user_ctx, message.c_str());
        }
    });
    p->impl->OnIncomingJson([p](const cJSON* root) {
        if (p->callbacks.on_json) {
            char* raw = cJSON_PrintUnformatted(root);
            p->callbacks.on_json(p->callbacks.user_ctx, raw ? raw : "");
            if (raw) {
                cJSON_free(raw);
            }
        }
    });
    p->impl->OnIncomingAudio([p](std::unique_ptr<AudioStreamPacket> packet) {
        if (!p->callbacks.on_audio || !packet) {
            return;
        }
        protocol_audio_packet_t c_pkt = {
            .sample_rate = packet->sample_rate,
            .frame_duration = packet->frame_duration,
            .timestamp = packet->timestamp,
            .payload = packet->payload.data(),
            .payload_size = packet->payload.size(),
        };
        p->callbacks.on_audio(p->callbacks.user_ctx, &c_pkt);
    });
}

protocol_handle_t *protocol_create(const protocol_config_t *cfg) {
    if (cfg == nullptr) {
        return nullptr;
    }
    std::unique_ptr<protocol_handle_t> p(new (std::nothrow) protocol_handle_t{});
    if (!p) {
        return nullptr;
    }
    p->callbacks = cfg->callbacks;
    if (cfg->kind == PROTOCOL_KIND_WEBSOCKET) {
        p->impl = std::make_unique<WebsocketProtocol>();
    } else {
        p->impl = std::make_unique<MqttProtocol>();
    }
    wire_callbacks(p.get());
    return p.release();
}

void protocol_destroy(protocol_handle_t *p) {
    delete p;
}

esp_err_t protocol_start(protocol_handle_t *p) {
    if (p == nullptr || !p->impl) {
        return ESP_ERR_INVALID_ARG;
    }
    return p->impl->Start() ? ESP_OK : ESP_FAIL;
}

esp_err_t protocol_open_audio_channel(protocol_handle_t *p) {
    if (p == nullptr || !p->impl) {
        return ESP_ERR_INVALID_ARG;
    }
    return p->impl->OpenAudioChannel() ? ESP_OK : ESP_FAIL;
}

esp_err_t protocol_close_audio_channel(protocol_handle_t *p, bool send_goodbye) {
    if (p == nullptr || !p->impl) {
        return ESP_ERR_INVALID_ARG;
    }
    p->impl->CloseAudioChannel(send_goodbye);
    return ESP_OK;
}

bool protocol_is_audio_channel_opened(const protocol_handle_t *p) {
    return p && p->impl && p->impl->IsAudioChannelOpened();
}

esp_err_t protocol_send_audio(protocol_handle_t *p, const protocol_audio_packet_t *packet) {
    if (p == nullptr || !p->impl || packet == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto audio = std::make_unique<AudioStreamPacket>();
    audio->sample_rate = packet->sample_rate;
    audio->frame_duration = packet->frame_duration;
    audio->timestamp = packet->timestamp;
    audio->payload.assign(packet->payload, packet->payload + packet->payload_size);
    return p->impl->SendAudio(std::move(audio)) ? ESP_OK : ESP_FAIL;
}

esp_err_t protocol_send_wake_word_detected(protocol_handle_t *p, const char *wake_word) {
    if (p == nullptr || !p->impl || wake_word == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    p->impl->SendWakeWordDetected(wake_word);
    return ESP_OK;
}

esp_err_t protocol_send_start_listening(protocol_handle_t *p, listen_mode_t mode) {
    if (p == nullptr || !p->impl) {
        return ESP_ERR_INVALID_ARG;
    }
    ListeningMode m = kListeningModeAutoStop;
    if (mode == LISTEN_MODE_MANUALSTOP) {
        m = kListeningModeManualStop;
    } else if (mode == LISTEN_MODE_REALTIME) {
        m = kListeningModeRealtime;
    }
    p->impl->SendStartListening(m);
    return ESP_OK;
}

esp_err_t protocol_send_stop_listening(protocol_handle_t *p) {
    if (p == nullptr || !p->impl) {
        return ESP_ERR_INVALID_ARG;
    }
    p->impl->SendStopListening();
    return ESP_OK;
}

esp_err_t protocol_send_abort_speaking(protocol_handle_t *p, int reason) {
    if (p == nullptr || !p->impl) {
        return ESP_ERR_INVALID_ARG;
    }
    AbortReason r = reason == 0 ? kAbortReasonNone : kAbortReasonWakeWordDetected;
    p->impl->SendAbortSpeaking(r);
    return ESP_OK;
}

esp_err_t protocol_send_mcp_message(protocol_handle_t *p, const char *message_json) {
    if (p == nullptr || !p->impl || message_json == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    p->impl->SendMcpMessage(message_json);
    return ESP_OK;
}
