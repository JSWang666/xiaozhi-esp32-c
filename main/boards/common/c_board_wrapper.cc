#include "board_defs.h"
#include "wifi_board.h"
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_ESP32
#include "ml307_board.h"
#include "nt26_board.h"
#include "dual_network_board.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "rndis_board.h"
#endif
#include "application.h"

#include "led/led.h"
#include "audio/codecs/no_audio_codec.h"
#include "audio/audio_codec.h"
#include "display/display.h"
#include "backlight.h"

#include <esp_log.h>
#include <driver/gpio.h>

#include <utility>

static const char *TAG = "CBoardWrapper";

/* Forward-declared in display_cpp_bridge.cc */
Display *display_unwrap_cpp(display_t *d);

namespace {

/* ===== Adapter classes ===== */

/* Wraps a C led_t* into the abstract Led base class. Forwards OnStateChanged()
 * through the C ops table so SingleLed / GpioLed / CircularStrip C
 * implementations all light up properly on state changes. */
class CLedAdapter : public Led {
public:
    explicit CLedAdapter(led_t *c_led) : Led(c_led) {}
    void OnStateChanged() override {
        if (c_led_ && c_led_->ops && c_led_->ops->on_state_changed) {
            auto &app = Application::GetInstance();
            c_led_->ops->on_state_changed(c_led_, app.GetDeviceState(),
                                          app.IsVoiceDetected());
        }
    }
};

/* Wraps a raw audio_codec_t* via NoAudioCodec (which already forwards
 * Read/Write/EnableInput/EnableOutput to ops). We add the missing
 * SetOutputVolume / SetInputGain / Start overrides so that boards with a
 * pure-C codec (sensecap / m5stack / df-k10 / lilygo ADC-PDM / etc.) get
 * volume / gain / start actually applied to the hardware. */
class CAudioCodecAdapter : public NoAudioCodec {
public:
    explicit CAudioCodecAdapter(audio_codec_t *c) : NoAudioCodec(c) {}

    void Start() override {
        AudioCodec::Start();   /* load output_volume_ from settings */
        if (c_codec_) {
            /* Propagate the loaded volume into the C struct so codec
             * ops (which read codec->output_volume) see the right value. */
            c_codec_->output_volume = output_volume_;
            if (c_codec_->ops && c_codec_->ops->start) {
                c_codec_->ops->start(c_codec_);
            }
        }
    }

    void SetOutputVolume(int volume) override {
        if (c_codec_) {
            if (c_codec_->ops && c_codec_->ops->set_output_volume) {
                c_codec_->ops->set_output_volume(c_codec_, volume);
            } else {
                /* Ops not provided — at least update struct + persist. */
                audio_codec_base_set_output_volume(c_codec_, volume);
            }
            output_volume_ = c_codec_->output_volume;
            return;
        }
        AudioCodec::SetOutputVolume(volume);
    }

    void SetInputGain(float gain) override {
        if (c_codec_) {
            if (c_codec_->ops && c_codec_->ops->set_input_gain) {
                c_codec_->ops->set_input_gain(c_codec_, gain);
            } else {
                audio_codec_base_set_input_gain(c_codec_, gain);
            }
            input_gain_ = c_codec_->input_gain;
            return;
        }
        AudioCodec::SetInputGain(gain);
    }
};

/* ===== Shared delegate that holds the C board_desc_t* and adapter cache.
 * Used as a member of every CBoardWrapper<T> so all wrappers share the
 * exact same getter logic regardless of which Board base they extend. ===== */
class CBoardDelegate {
public:
    explicit CBoardDelegate(board_desc_t *d) : desc_(d) {}
    ~CBoardDelegate() {
        /* Adapters reference data that the board's destroy() will free,
         * so delete adapters first. */
        delete led_adapter_;
        delete codec_adapter_;
        delete backlight_adapter_;
        if (desc_ && desc_->destroy) {
            desc_->destroy(desc_);
        }
    }

    CBoardDelegate(const CBoardDelegate &) = delete;
    CBoardDelegate &operator=(const CBoardDelegate &) = delete;

    board_desc_t *desc() const { return desc_; }

    std::string GetBoardType(const std::string &fallback) {
        if (desc_ && desc_->get_board_type) {
            const char *s = desc_->get_board_type(desc_);
            if (s) return s;
        }
        return fallback;
    }

    Led *GetLed(Led *fallback) {
        if (led_adapter_) return led_adapter_;
        if (!desc_ || !desc_->get_led) return fallback;
        led_t *c = (led_t *)desc_->get_led(desc_);
        if (!c) return fallback;
        led_adapter_ = new CLedAdapter(c);
        return led_adapter_;
    }

    AudioCodec *GetAudioCodec(AudioCodec *fallback) {
        if (codec_adapter_) return codec_adapter_;
        if (!desc_ || !desc_->get_audio_codec) return fallback;
        audio_codec_t *c = (audio_codec_t *)desc_->get_audio_codec(desc_);
        if (!c) return fallback;
        codec_adapter_ = new CAudioCodecAdapter(c);
        return codec_adapter_;
    }

    Display *GetDisplay(Display *fallback) {
        if (!desc_ || !desc_->get_display) return fallback;
        display_t *c = (display_t *)desc_->get_display(desc_);
        if (!c) return fallback;
        Display *impl = display_unwrap_cpp(c);
        if (impl) return impl;
        ESP_LOGW(TAG, "display_t at %p was not created via display_cpp_bridge; "
                       "falling back to NoDisplay", c);
        return fallback;
    }

    Backlight *GetBacklight(Backlight *fallback) {
        if (backlight_adapter_) return backlight_adapter_;
        if (!desc_ || !desc_->get_backlight) return fallback;
        backlight_t *c = (backlight_t *)desc_->get_backlight(desc_);
        if (!c) return fallback;
        backlight_adapter_ = new Backlight(c);
        return backlight_adapter_;
    }

    bool GetBatteryLevel(int &level, bool &charging, bool &discharging) {
        if (desc_ && desc_->get_battery_level) {
            return desc_->get_battery_level(desc_, &level, &charging, &discharging);
        }
        return false;
    }

    bool GetTemperature(float &temp) {
        if (desc_ && desc_->get_temperature) {
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

/* ===== Template wrapper that grafts a board_desc_t onto any concrete Board
 * subclass (WifiBoard / Ml307Board / Nt26Board / RndisBoard /
 * DualNetworkBoard). Constructor arguments after the desc are forwarded to
 * the Board subclass ctor. ===== */
template <class BaseBoard>
class CBoardWrapper : public BaseBoard {
public:
    template <typename... Args>
    explicit CBoardWrapper(board_desc_t *d, Args &&...args)
        : BaseBoard(std::forward<Args>(args)...), del_(d) {}

    std::string GetBoardType() override {
        return del_.GetBoardType(BaseBoard::GetBoardType());
    }
    Led *GetLed() override {
        return del_.GetLed(Board::GetLed());
    }
    AudioCodec *GetAudioCodec() override {
        /* Use nullptr as fallback rather than BaseBoard::GetAudioCodec(): some
         * Board subclasses (notably DualNetworkBoard) leave GetAudioCodec
         * pure-virtual, so a static reference to it would not link. A C board
         * is always expected to populate get_audio_codec. */
        return del_.GetAudioCodec(nullptr);
    }
    Display *GetDisplay() override {
        return del_.GetDisplay(Board::GetDisplay());
    }
    Backlight *GetBacklight() override {
        return del_.GetBacklight(Board::GetBacklight());
    }
    bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        if (del_.GetBatteryLevel(level, charging, discharging)) return true;
        return BaseBoard::GetBatteryLevel(level, charging, discharging);
    }
    bool GetTemperature(float &temp) override {
        if (del_.GetTemperature(temp)) return true;
        return BaseBoard::GetTemperature(temp);
    }

private:
    CBoardDelegate del_;
};

}  // namespace

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
        return new CBoardWrapper<WifiBoard>(desc);

#if !CONFIG_IDF_TARGET_ESP32
    case BOARD_KIND_ML307:
        return new CBoardWrapper<Ml307Board>(desc,
            (gpio_num_t)desc->modem_tx_pin,
            (gpio_num_t)desc->modem_rx_pin,
            (gpio_num_t)desc->modem_dtr_pin);

    case BOARD_KIND_NT26:
        return new CBoardWrapper<Nt26Board>(desc,
            (gpio_num_t)desc->modem_tx_pin,
            (gpio_num_t)desc->modem_rx_pin,
            (gpio_num_t)desc->modem_dtr_pin,
            (gpio_num_t)desc->modem_ri_pin,
            (gpio_num_t)desc->modem_reset_pin);

    case BOARD_KIND_DUAL:
        return new CBoardWrapper<DualNetworkBoard>(desc,
            (gpio_num_t)desc->modem_tx_pin,
            (gpio_num_t)desc->modem_rx_pin,
            (gpio_num_t)desc->modem_dtr_pin,
            (int32_t)desc->default_net_type);
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    case BOARD_KIND_RNDIS:
        return new CBoardWrapper<RndisBoard>(desc);
#endif

    default:
        ESP_LOGE(TAG, "Unknown or unsupported board kind: %d", (int)desc->kind);
        if (desc->destroy) desc->destroy(desc);
        return nullptr;
    }
}
