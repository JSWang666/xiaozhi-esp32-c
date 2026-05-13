#include "board_defs.h"
#include "wifi_board.h"
#include "application.h"

#include "led/led.h"
#include "audio/codecs/no_audio_codec.h"
#include "display/display.h"
#include "backlight.h"

#include <esp_log.h>

static const char *TAG = "CBoardWrapper";

/* Forward-declared in display_cpp_bridge.cc */
Display *display_unwrap_cpp(display_t *d);

namespace {

/* C++ adapter that wraps a C led_t* into the abstract Led base class.
 * Forwards OnStateChanged() through the C ops table. */
class CLedAdapter : public Led {
public:
    explicit CLedAdapter(led_t *c_led) : Led(c_led) {}
    void OnStateChanged() override {
        if (c_led_ && c_led_->ops && c_led_->ops->on_state_changed) {
            auto &app = Application::GetInstance();
            c_led_->ops->on_state_changed(c_led_, app.GetDeviceState(), app.IsVoiceDetected());
        }
    }
};

/* C++ adapter wrapping a raw audio_codec_t* via the concrete NoAudioCodec
 * base, which already forwards Read/Write/EnableInput/EnableOutput through
 * the C ops table. Works for any codec whose ops are correctly populated. */
class CAudioCodecAdapter : public NoAudioCodec {
public:
    explicit CAudioCodecAdapter(audio_codec_t *c) : NoAudioCodec(c) {}
};

/* Adapter classes for Display are not needed: oled_display_create() and
 * friends already construct a real C++ Display internally and wrap it in
 * a display_t struct. We just unwrap it. */

} // namespace

class CBoardWifiWrapper : public WifiBoard {
public:
    explicit CBoardWifiWrapper(board_desc_t *desc) : desc_(desc) {}
    ~CBoardWifiWrapper() override {
        delete led_adapter_;
        delete codec_adapter_;
        delete backlight_adapter_;
        if (desc_ && desc_->destroy) {
            desc_->destroy(desc_);
        }
    }

    std::string GetBoardType() override {
        if (desc_->get_board_type) {
            return desc_->get_board_type(desc_);
        }
        return WifiBoard::GetBoardType();
    }

    Led *GetLed() override {
        if (led_adapter_) return led_adapter_;
        if (!desc_->get_led) return Board::GetLed();
        led_t *c_led = static_cast<led_t *>(desc_->get_led(desc_));
        if (!c_led) return Board::GetLed();
        led_adapter_ = new CLedAdapter(c_led);
        return led_adapter_;
    }

    AudioCodec *GetAudioCodec() override {
        if (codec_adapter_) return codec_adapter_;
        if (!desc_->get_audio_codec) return WifiBoard::GetAudioCodec();
        audio_codec_t *c_codec = static_cast<audio_codec_t *>(desc_->get_audio_codec(desc_));
        if (!c_codec) return WifiBoard::GetAudioCodec();
        codec_adapter_ = new CAudioCodecAdapter(c_codec);
        return codec_adapter_;
    }

    Display *GetDisplay() override {
        if (!desc_->get_display) return Board::GetDisplay();
        display_t *c_disp = static_cast<display_t *>(desc_->get_display(desc_));
        if (!c_disp) return Board::GetDisplay();
        Display *impl = display_unwrap_cpp(c_disp);
        if (impl) return impl;
        ESP_LOGW(TAG, "display_t at %p was not created via display_cpp_bridge; "
                       "falling back to NoDisplay", c_disp);
        return Board::GetDisplay();
    }

    Backlight *GetBacklight() override {
        if (backlight_adapter_) return backlight_adapter_;
        if (!desc_->get_backlight) return Board::GetBacklight();
        backlight_t *c_bl = static_cast<backlight_t *>(desc_->get_backlight(desc_));
        if (!c_bl) return Board::GetBacklight();
        backlight_adapter_ = new Backlight(c_bl);
        return backlight_adapter_;
    }

    Camera *GetCamera() override {
        /* No generic C-struct camera abstraction exists yet. C boards that
         * called esp_camera_init() lose the Camera* interface during the
         * C migration. Return the default (null/no camera) from base Board. */
        return Board::GetCamera();
    }

    bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        if (desc_->get_battery_level) {
            return desc_->get_battery_level(desc_, &level, &charging, &discharging);
        }
        return false;
    }

    bool GetTemperature(float &temp) override {
        if (desc_->get_temperature) {
            return desc_->get_temperature(desc_, &temp);
        }
        return false;
    }

private:
    board_desc_t *desc_;
    Led *led_adapter_ = nullptr;
    AudioCodec *codec_adapter_ = nullptr;
    Backlight *backlight_adapter_ = nullptr;
};

extern "C" __attribute__((weak)) board_desc_t *create_board_desc(void)
{
    return nullptr;
}

__attribute__((weak)) void *create_board()
{
    board_desc_t *desc = create_board_desc();
    if (!desc) {
        ESP_LOGE(TAG, "create_board_desc() returned NULL");
        return nullptr;
    }

    switch (desc->kind) {
    case BOARD_KIND_WIFI:
    default:
        return new CBoardWifiWrapper(desc);
    }
}
