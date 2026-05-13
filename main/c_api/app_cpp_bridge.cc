#include "app_c_api.h"

#include <new>

#include "application.h"

struct app_context {
    app_config_t cfg;
    bool initialized;
};

static app_context_t *s_app_ctx = nullptr;

app_context_t *app_get_context(void) {
    return s_app_ctx;
}

app_context_t *app_create(const app_config_t *cfg) {
    app_context_t *ctx = new (std::nothrow) app_context_t{};
    if (ctx == nullptr) {
        return nullptr;
    }
    if (cfg != nullptr) {
        ctx->cfg = *cfg;
    }
    s_app_ctx = ctx;
    return ctx;
}

void app_destroy(app_context_t *ctx) {
    if (ctx == s_app_ctx) s_app_ctx = nullptr;
    delete ctx;
}

esp_err_t app_init(app_context_t *ctx) {
    if (ctx == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    Application::GetInstance().Initialize();
    ctx->initialized = true;
    return ESP_OK;
}

esp_err_t app_run(app_context_t *ctx) {
    if (ctx == nullptr || !ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    Application::GetInstance().Run();
    return ESP_OK;
}

esp_err_t app_post_event(app_context_t *ctx, app_event_t event, const void *event_data) {
    (void)event_data;
    if (ctx == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (event) {
        case APP_EVENT_TOGGLE_CHAT:
            Application::GetInstance().ToggleChatState();
            return ESP_OK;
        case APP_EVENT_START_LISTENING:
            Application::GetInstance().StartListening();
            return ESP_OK;
        case APP_EVENT_STOP_LISTENING:
            Application::GetInstance().StopListening();
            return ESP_OK;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t app_schedule(app_context_t *ctx, app_task_fn task, void *arg) {
    if (ctx == nullptr || task == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    Application::GetInstance().Schedule([task, arg]() { task(arg); });
    return ESP_OK;
}

esp_err_t app_toggle_chat(app_context_t *ctx) {
    return app_post_event(ctx, APP_EVENT_TOGGLE_CHAT, nullptr);
}

esp_err_t app_start_listening(app_context_t *ctx) {
    return app_post_event(ctx, APP_EVENT_START_LISTENING, nullptr);
}

esp_err_t app_stop_listening(app_context_t *ctx) {
    return app_post_event(ctx, APP_EVENT_STOP_LISTENING, nullptr);
}

esp_err_t app_reboot(app_context_t *ctx) {
    if (ctx == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().Reboot();
    return ESP_OK;
}

esp_err_t app_abort_speaking(app_context_t *ctx, int reason) {
    if (ctx == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().AbortSpeaking(static_cast<AbortReason>(reason));
    return ESP_OK;
}

esp_err_t app_alert(app_context_t *ctx, const char *status, const char *message, const char *emotion, const char *sound) {
    if (ctx == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().Alert(
        status ? status : "",
        message ? message : "",
        emotion ? emotion : "",
        sound ? std::string_view(sound) : std::string_view());
    return ESP_OK;
}

esp_err_t app_dismiss_alert(app_context_t *ctx) {
    if (ctx == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().DismissAlert();
    return ESP_OK;
}

esp_err_t app_play_sound(app_context_t *ctx, const char *sound_name) {
    if (ctx == nullptr || sound_name == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().PlaySound(sound_name);
    return ESP_OK;
}

int app_get_device_state(app_context_t *ctx) {
    if (ctx == nullptr) return -1;
    return static_cast<int>(Application::GetInstance().GetDeviceState());
}

bool app_set_device_state(app_context_t *ctx, int state) {
    if (ctx == nullptr) return false;
    return Application::GetInstance().SetDeviceState(static_cast<DeviceState>(state));
}

bool app_is_voice_detected(app_context_t *ctx) {
    if (ctx == nullptr) return false;
    return Application::GetInstance().IsVoiceDetected();
}

bool app_can_enter_sleep_mode(app_context_t *ctx) {
    if (ctx == nullptr) return false;
    return Application::GetInstance().CanEnterSleepMode();
}

esp_err_t app_wake_word_invoke(app_context_t *ctx, const char *wake_word) {
    if (ctx == nullptr || wake_word == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().WakeWordInvoke(std::string(wake_word));
    return ESP_OK;
}

esp_err_t app_send_mcp_message(app_context_t *ctx, const char *payload) {
    if (ctx == nullptr || payload == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().SendMcpMessage(std::string(payload));
    return ESP_OK;
}

esp_err_t app_set_aec_mode(app_context_t *ctx, int mode) {
    if (ctx == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().SetAecMode(static_cast<AecMode>(mode));
    return ESP_OK;
}

int app_get_aec_mode(app_context_t *ctx) {
    if (ctx == nullptr) return -1;
    return static_cast<int>(Application::GetInstance().GetAecMode());
}

esp_err_t app_reset_protocol(app_context_t *ctx) {
    if (ctx == nullptr) return ESP_ERR_INVALID_ARG;
    Application::GetInstance().ResetProtocol();
    return ESP_OK;
}

bool app_upgrade_firmware(app_context_t *ctx, const char *url, const char *version) {
    if (ctx == nullptr || url == nullptr) return false;
    return Application::GetInstance().UpgradeFirmware(
        std::string(url),
        version ? std::string(version) : std::string());
}
