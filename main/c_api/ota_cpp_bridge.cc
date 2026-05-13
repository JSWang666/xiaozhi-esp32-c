#include "ota_c_api.h"
#include "ota.h"

#include <new>

struct ota_handle {
    Ota impl;
};

ota_handle_t *ota_create(void) {
    return new (std::nothrow) ota_handle_t{};
}

void ota_destroy(ota_handle_t *ota) {
    delete ota;
}

esp_err_t ota_check_version(ota_handle_t *ota) {
    if (ota == nullptr) return ESP_ERR_INVALID_ARG;
    return ota->impl.CheckVersion();
}

esp_err_t ota_activate(ota_handle_t *ota) {
    if (ota == nullptr) return ESP_ERR_INVALID_ARG;
    return ota->impl.Activate();
}

void ota_mark_current_version_valid(ota_handle_t *ota) {
    if (ota) ota->impl.MarkCurrentVersionValid();
}

bool ota_has_new_version(ota_handle_t *ota) {
    return ota && ota->impl.HasNewVersion();
}

bool ota_has_mqtt_config(ota_handle_t *ota) {
    return ota && ota->impl.HasMqttConfig();
}

bool ota_has_websocket_config(ota_handle_t *ota) {
    return ota && ota->impl.HasWebsocketConfig();
}

bool ota_has_activation_code(ota_handle_t *ota) {
    return ota && ota->impl.HasActivationCode();
}

bool ota_has_activation_challenge(ota_handle_t *ota) {
    return ota && ota->impl.HasActivationChallenge();
}

bool ota_has_server_time(ota_handle_t *ota) {
    return ota && ota->impl.HasServerTime();
}

const char *ota_get_firmware_version(ota_handle_t *ota) {
    if (ota == nullptr) return "";
    return ota->impl.GetFirmwareVersion().c_str();
}

const char *ota_get_current_version(ota_handle_t *ota) {
    if (ota == nullptr) return "";
    return ota->impl.GetCurrentVersion().c_str();
}

const char *ota_get_firmware_url(ota_handle_t *ota) {
    if (ota == nullptr) return "";
    return ota->impl.GetFirmwareUrl().c_str();
}

const char *ota_get_activation_message(ota_handle_t *ota) {
    if (ota == nullptr) return "";
    return ota->impl.GetActivationMessage().c_str();
}

const char *ota_get_activation_code(ota_handle_t *ota) {
    if (ota == nullptr) return "";
    return ota->impl.GetActivationCode().c_str();
}

const char *ota_get_check_version_url(ota_handle_t *ota) {
    if (ota == nullptr) return "";
    static std::string url;
    url = ota->impl.GetCheckVersionUrl();
    return url.c_str();
}

bool ota_start_upgrade(ota_handle_t *ota, ota_progress_cb_t cb, void *ctx) {
    if (ota == nullptr) return false;
    return ota->impl.StartUpgrade([cb, ctx](int progress, size_t speed) {
        if (cb) cb(progress, speed, ctx);
    });
}

bool ota_upgrade_from_url(const char *url, ota_progress_cb_t cb, void *ctx) {
    if (url == nullptr) return false;
    return Ota::Upgrade(std::string(url), [cb, ctx](int progress, size_t speed) {
        if (cb) cb(progress, speed, ctx);
    });
}
