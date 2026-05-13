#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    kAbortReasonNone = 0,
    kAbortReasonWakeWordDetected
} abort_reason_t;

typedef enum {
    kListeningModeAutoStop = 0,
    kListeningModeManualStop,
    kListeningModeRealtime
} listening_mode_t;

typedef struct {
    int sample_rate;
    int frame_duration;
    uint32_t timestamp;
    uint8_t *payload;
    size_t payload_size;
} audio_stream_packet_t;

struct BinaryProtocol2 {
    uint16_t version;
    uint16_t type;
    uint32_t reserved;
    uint32_t timestamp;
    uint32_t payload_size;
    uint8_t payload[];
} __attribute__((packed));

struct BinaryProtocol3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} __attribute__((packed));

typedef void (*protocol_on_audio_cb_t)(audio_stream_packet_t *packet, void *ctx);
typedef void (*protocol_on_json_cb_t)(const cJSON *root, void *ctx);
typedef void (*protocol_on_event_cb_t)(void *ctx);
typedef void (*protocol_on_error_cb_t)(const char *message, void *ctx);

typedef struct protocol_ops {
    bool (*start)(void *impl);
    bool (*open_audio_channel)(void *impl);
    void (*close_audio_channel)(void *impl, bool send_goodbye);
    bool (*is_audio_channel_opened)(void *impl);
    bool (*send_audio)(void *impl, audio_stream_packet_t *packet);
    void (*send_wake_word_detected)(void *impl, const char *wake_word);
    void (*send_start_listening)(void *impl, listening_mode_t mode);
    void (*send_stop_listening)(void *impl);
    void (*send_abort_speaking)(void *impl, abort_reason_t reason);
    void (*send_mcp_message)(void *impl, const char *message);
    int (*get_server_sample_rate)(void *impl);
    int (*get_server_frame_duration)(void *impl);
    const char *(*get_session_id)(void *impl);
    void (*set_on_audio_cb)(void *impl, protocol_on_audio_cb_t cb, void *ctx);
    void (*set_on_json_cb)(void *impl, protocol_on_json_cb_t cb, void *ctx);
    void (*set_on_audio_channel_opened_cb)(void *impl, protocol_on_event_cb_t cb, void *ctx);
    void (*set_on_audio_channel_closed_cb)(void *impl, protocol_on_event_cb_t cb, void *ctx);
    void (*set_on_network_error_cb)(void *impl, protocol_on_error_cb_t cb, void *ctx);
    void (*set_on_connected_cb)(void *impl, protocol_on_event_cb_t cb, void *ctx);
    void (*set_on_disconnected_cb)(void *impl, protocol_on_event_cb_t cb, void *ctx);
    void (*destroy)(void *impl);
} protocol_ops_t;

typedef struct c_protocol {
    const protocol_ops_t *ops;
} c_protocol_t;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
#include <functional>
#include <chrono>
#include <vector>
#include <memory>

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

typedef abort_reason_t AbortReason;
typedef listening_mode_t ListeningMode;

class Protocol {
public:
    virtual ~Protocol() = default;

    inline int server_sample_rate() const {
        return server_sample_rate_;
    }
    inline int server_frame_duration() const {
        return server_frame_duration_;
    }
    inline const std::string& session_id() const {
        return session_id_;
    }

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnNetworkError(std::function<void(const std::string& message)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;
    virtual void SendWakeWordDetected(const std::string& wake_word);
    virtual void SendStartListening(ListeningMode mode);
    virtual void SendStopListening();
    virtual void SendAbortSpeaking(AbortReason reason);
    virtual void SendMcpMessage(const std::string& message);

    c_protocol_t *c_protocol_ = nullptr;

protected:
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    bool error_occurred_ = false;
    std::string session_id_;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    virtual bool SendText(const std::string& text) = 0;
    virtual void SetError(const std::string& message);
    virtual bool IsTimeout() const;
};
#endif

#endif // PROTOCOL_H
