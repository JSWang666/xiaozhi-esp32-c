#include "cst816x.h"
#include "c_api/app_c_api.h"
#include "device_state.h"
#include "i2c_device.h"
#include "assets/lang_c.h"
#include "backlight.h"

#include <esp_log.h>
#include <string.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Cst816x"

#define ES8311_VOL_MIN 0
#define ES8311_VOL_MAX 100

typedef enum {
    TOUCH_EVT_SINGLE_CLICK,
    TOUCH_EVT_DOUBLE_CLICK,
    TOUCH_EVT_LONG_PRESS_START,
    TOUCH_EVT_LONG_PRESS_END,
} touch_event_type_t;

typedef struct {
    touch_event_type_t type;
    int x;
    int y;
} touch_event_t;

typedef struct {
    int x, y;
    int64_t single_click_thresh_us;
    int64_t double_click_window_us;
    int64_t long_press_thresh_us;
} touch_threshold_config_t;

static const touch_threshold_config_t DEFAULT_THRESHOLD = {
    .x = -1, .y = -1,
    .single_click_thresh_us = 120000,
    .double_click_window_us = 240000,
    .long_press_thresh_us = 4000000,
};

static const touch_threshold_config_t TOUCH_THRESHOLD_TABLE[] = {
    { 20, 600, 200000, 240000, 2000000 },
    { 40, 600, 200000, 240000, 4000000 },
    { 60, 600, 200000, 240000, 2000000 },
};

typedef struct {
    i2c_device_t *dev;
    uint8_t read_buffer[6];
    int tp_num, tp_x, tp_y;

    bool is_touching;
    int64_t touch_start_time;
    int64_t last_release_time;
    int click_count;
    bool long_press_started;

    bool is_volume_long_pressing;
    int volume_long_press_dir;
    int64_t last_volume_adjust_time;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;
} cst816x_state_t;

static const int64_t VOL_ADJ_INTERVAL_US = 200000;
static const int VOL_ADJ_STEP = 5;

static const touch_threshold_config_t *get_threshold_config(int x, int y)
{
    for (int i = 0; i < 3; i++) {
        if (TOUCH_THRESHOLD_TABLE[i].x == x && TOUCH_THRESHOLD_TABLE[i].y == y)
            return &TOUCH_THRESHOLD_TABLE[i];
    }
    return &DEFAULT_THRESHOLD;
}

static int64_t get_current_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000L + tv.tv_usec;
}

static void update_touch_point(cst816x_state_t *s)
{
    i2c_device_read_regs(s->dev, 0x02, s->read_buffer, 6);
    s->tp_num = s->read_buffer[0] & 0x0F;
    s->tp_x = ((s->read_buffer[1] & 0x0F) << 8) | s->read_buffer[2];
    s->tp_y = ((s->read_buffer[3] & 0x0F) << 8) | s->read_buffer[4];
    memset(s->read_buffer, 0, 6);
}

static void touchpad_daemon(void *arg)
{
    cst816x_state_t *s = (cst816x_state_t *)arg;

    while (1) {
        update_touch_point(s);
        int64_t current_time = get_current_time_us();

        const touch_threshold_config_t *config = get_threshold_config(s->tp_x, s->tp_y);

        touch_event_t current_event = { 0 };
        bool event_detected = false;

        if (s->tp_num > 0 && !s->is_touching) {
            s->is_touching = true;
            s->touch_start_time = current_time;
            s->long_press_started = false;
        } else if (s->tp_num > 0 && s->is_touching) {
            if (!s->long_press_started &&
                (current_time - s->touch_start_time >= config->long_press_thresh_us)) {
                current_event.type = TOUCH_EVT_LONG_PRESS_START;
                current_event.x = s->tp_x;
                current_event.y = s->tp_y;
                event_detected = true;
                s->long_press_started = true;
            }
        } else if (s->tp_num == 0 && s->is_touching) {
            s->is_touching = false;
            int64_t touch_duration = current_time - s->touch_start_time;
            s->last_release_time = current_time;
            if (s->long_press_started) {
                current_event.type = TOUCH_EVT_LONG_PRESS_END;
                current_event.x = s->tp_x;
                current_event.y = s->tp_y;
                event_detected = true;
            } else if (touch_duration <= config->single_click_thresh_us) {
                s->click_count++;
            }
        } else if (s->tp_num == 0 && !s->is_touching) {
            if (s->click_count > 0 &&
                (current_time - s->last_release_time >= config->double_click_window_us)) {
                if (s->click_count == 2) {
                    current_event.type = TOUCH_EVT_DOUBLE_CLICK;
                } else if (s->click_count == 1) {
                    current_event.type = TOUCH_EVT_SINGLE_CLICK;
                }
                current_event.x = s->tp_x;
                current_event.y = s->tp_y;
                event_detected = true;
                s->click_count = 0;
            }
        }

        if (event_detected && current_event.y == 600 &&
            (current_event.x == 20 || current_event.x == 40 || current_event.x == 60)) {

            audio_codec_t *codec = s->codec;

            switch (current_event.type) {
            case TOUCH_EVT_SINGLE_CLICK:
                if (current_event.x == 40) {
                    app_context_t *app = app_get_context();
                    if (app) {
                        if (app_get_device_state(app) == kDeviceStateStarting) return;
                        app_toggle_chat(app);
                    }
                } else if (current_event.x == 20 && codec) {
                    int new_vol = codec->output_volume + 10;
                    if (new_vol > ES8311_VOL_MAX) new_vol = ES8311_VOL_MAX;
                    if (codec->ops && codec->ops->enable_output)
                        codec->ops->enable_output(codec, true);
                    if (codec->ops && codec->ops->set_output_volume)
                        codec->ops->set_output_volume(codec, new_vol);
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, new_vol);
                    if (s->display)
                        display_show_notification(s->display, buf, 0);
                } else if (current_event.x == 60 && codec) {
                    int new_vol = codec->output_volume - 10;
                    if (new_vol < ES8311_VOL_MIN) new_vol = ES8311_VOL_MIN;
                    if (codec->ops && codec->ops->enable_output)
                        codec->ops->enable_output(codec, true);
                    if (codec->ops && codec->ops->set_output_volume)
                        codec->ops->set_output_volume(codec, new_vol);
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, new_vol);
                    if (s->display)
                        display_show_notification(s->display, buf, 0);
                }
                break;

            case TOUCH_EVT_LONG_PRESS_START:
                if (current_event.x == 20) {
                    s->is_volume_long_pressing = true;
                    s->volume_long_press_dir = 1;
                    s->last_volume_adjust_time = current_time;
                } else if (current_event.x == 60) {
                    s->is_volume_long_pressing = true;
                    s->volume_long_press_dir = -1;
                    s->last_volume_adjust_time = current_time;
                }
                break;

            case TOUCH_EVT_LONG_PRESS_END:
                if (current_event.x == 20 || current_event.x == 60) {
                    s->is_volume_long_pressing = false;
                    s->volume_long_press_dir = 0;
                }
                break;

            default:
                break;
            }
        }

        if (s->is_volume_long_pressing && s->codec) {
            int64_t now = get_current_time_us();
            if (now - s->last_volume_adjust_time >= VOL_ADJ_INTERVAL_US) {
                audio_codec_t *codec = s->codec;
                int new_vol = codec->output_volume + (s->volume_long_press_dir * VOL_ADJ_STEP);
                if (new_vol < ES8311_VOL_MIN) new_vol = ES8311_VOL_MIN;
                if (new_vol > ES8311_VOL_MAX) new_vol = ES8311_VOL_MAX;

                if (new_vol != codec->output_volume) {
                    if (codec->ops && codec->ops->enable_output)
                        codec->ops->enable_output(codec, true);
                    if (codec->ops && codec->ops->set_output_volume)
                        codec->ops->set_output_volume(codec, new_vol);
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s%d", lang_str_volume, new_vol);
                    if (s->display)
                        display_show_notification(s->display, buf, 0);
                    s->last_volume_adjust_time = now;
                } else {
                    s->is_volume_long_pressing = false;
                    s->volume_long_press_dir = 0;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void cst816x_init(i2c_master_bus_handle_t i2c_bus, uint8_t addr,
                  audio_codec_t *codec, display_t *display, backlight_t *backlight)
{
    cst816x_state_t *s = calloc(1, sizeof(cst816x_state_t));
    if (!s) return;

    s->dev = i2c_device_create(i2c_bus, addr);
    uint8_t chip_id = i2c_device_read_reg(s->dev, 0xA7);
    ESP_LOGI(TAG, "Get CST816x chip ID: 0x%02X", chip_id);

    s->codec = codec;
    s->display = display;
    s->backlight = backlight;

    xTaskCreate(touchpad_daemon, "touch_daemon", 2048, s, 1, NULL);
}
