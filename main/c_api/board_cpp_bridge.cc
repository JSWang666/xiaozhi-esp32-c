#include "board_c_api.h"

#include <string>

#include "board.h"
#include "wifi_board.h"

struct board_handle {
    Board* impl;
    board_network_callback_t net_cb;
};

static board_handle_t s_board_handle = {};

board_handle_t *board_get_instance(void) {
    if (!s_board_handle.impl) {
        s_board_handle.impl = &Board::GetInstance();
    }
    return &s_board_handle;
}

static board_network_event_t to_c_event(NetworkEvent event) {
    switch (event) {
        case NetworkEvent::Scanning:
            return BOARD_NET_SCANNING;
        case NetworkEvent::Connecting:
            return BOARD_NET_CONNECTING;
        case NetworkEvent::Connected:
            return BOARD_NET_CONNECTED;
        case NetworkEvent::Disconnected:
            return BOARD_NET_DISCONNECTED;
        case NetworkEvent::WifiConfigModeEnter:
            return BOARD_NET_WIFI_CONFIG_MODE_ENTER;
        case NetworkEvent::WifiConfigModeExit:
            return BOARD_NET_WIFI_CONFIG_MODE_EXIT;
        case NetworkEvent::ModemDetecting:
            return BOARD_NET_MODEM_DETECTING;
        case NetworkEvent::ModemErrorNoSim:
            return BOARD_NET_MODEM_ERROR_NO_SIM;
        case NetworkEvent::ModemErrorRegDenied:
            return BOARD_NET_MODEM_ERROR_REG_DENIED;
        case NetworkEvent::ModemErrorInitFailed:
            return BOARD_NET_MODEM_ERROR_INIT_FAILED;
        case NetworkEvent::ModemErrorTimeout:
            return BOARD_NET_MODEM_ERROR_TIMEOUT;
    }
    return BOARD_NET_DISCONNECTED;
}

board_handle_t *board_create(const board_init_cfg_t *cfg) {
    (void)cfg;
    static board_handle_t singleton = {
        .impl = &Board::GetInstance(),
        .net_cb = {},
    };
    return &singleton;
}

void board_destroy(board_handle_t *board) {
    (void)board;
}

const char *board_get_type(board_handle_t *board) {
    if (board == nullptr || board->impl == nullptr) {
        return "";
    }
    static std::string board_type;
    board_type = board->impl->GetBoardType();
    return board_type.c_str();
}

const char *board_get_uuid(board_handle_t *board) {
    if (board == nullptr || board->impl == nullptr) {
        return "";
    }
    static std::string uuid;
    uuid = board->impl->GetUuid();
    return uuid.c_str();
}

void *board_get_audio_codec(board_handle_t *board) {
    return (board && board->impl) ? board->impl->GetAudioCodec() : nullptr;
}

void *board_get_display(board_handle_t *board) {
    return (board && board->impl) ? board->impl->GetDisplay() : nullptr;
}

void *board_get_led(board_handle_t *board) {
    return (board && board->impl) ? board->impl->GetLed() : nullptr;
}

void *board_get_backlight(board_handle_t *board) {
    return (board && board->impl) ? board->impl->GetBacklight() : nullptr;
}

void *board_get_camera(board_handle_t *board) {
    return (board && board->impl) ? board->impl->GetCamera() : nullptr;
}

void *board_get_network(board_handle_t *board) {
    return (board && board->impl) ? board->impl->GetNetwork() : nullptr;
}

esp_err_t board_start_network(board_handle_t *board) {
    if (board == nullptr || board->impl == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    board->impl->StartNetwork();
    return ESP_OK;
}

esp_err_t board_set_network_callback(board_handle_t *board, const board_network_callback_t *cb) {
    if (board == nullptr || board->impl == nullptr || cb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    board->net_cb = *cb;
    board->impl->SetNetworkEventCallback([board](NetworkEvent event, const std::string& data) {
        if (board->net_cb.cb != nullptr) {
            board->net_cb.cb(board->net_cb.user_ctx, to_c_event(event), data.c_str());
        }
    });
    return ESP_OK;
}

esp_err_t board_enter_wifi_config_mode(board_handle_t *board) {
    if (board == nullptr || board->impl == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *wifi = dynamic_cast<WifiBoard *>(board->impl);
    if (wifi == nullptr) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    wifi->EnterWifiConfigMode();
    return ESP_OK;
}

esp_err_t board_set_power_level(board_handle_t *board, board_power_level_t level) {
    if (board == nullptr || board->impl == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (level) {
        case BOARD_POWER_LOW:
            board->impl->SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            break;
        case BOARD_POWER_BALANCED:
            board->impl->SetPowerSaveLevel(PowerSaveLevel::BALANCED);
            break;
        case BOARD_POWER_PERFORMANCE:
            board->impl->SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

const char *board_get_network_state_icon(board_handle_t *board) {
    if (board == nullptr || board->impl == nullptr) return "";
    return board->impl->GetNetworkStateIcon();
}

bool board_get_battery_level(board_handle_t *board, int *level, bool *charging, bool *discharging) {
    if (board == nullptr || board->impl == nullptr) return false;
    return board->impl->GetBatteryLevel(*level, *charging, *discharging);
}

bool board_get_temperature(board_handle_t *board, float *temperature) {
    if (board == nullptr || board->impl == nullptr) return false;
    return board->impl->GetTemperature(*temperature);
}

const char *board_get_system_info_json(board_handle_t *board) {
    if (board == nullptr || board->impl == nullptr) return "{}";
    static std::string json;
    json = board->impl->GetSystemInfoJson();
    return json.c_str();
}

const char *board_get_board_json(board_handle_t *board) {
    if (board == nullptr || board->impl == nullptr) return "{}";
    static std::string json;
    json = board->impl->GetBoardJson();
    return json.c_str();
}

const char *board_get_device_status_json(board_handle_t *board) {
    if (board == nullptr || board->impl == nullptr) return "{}";
    static std::string json;
    json = board->impl->GetDeviceStatusJson();
    return json.c_str();
}
