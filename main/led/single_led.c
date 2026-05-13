#include "single_led.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <led_strip.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "SingleLed"
#define DEFAULT_BRIGHTNESS  4
#define HIGH_BRIGHTNESS     16
#define LOW_BRIGHTNESS_VAL  2
#define BLINK_INFINITE      (-1)

typedef struct {
    led_t base;
    led_strip_handle_t strip;
    esp_timer_handle_t blink_timer;
    SemaphoreHandle_t mutex;
    uint8_t r, g, b;
    int blink_counter;
    int blink_interval_ms;
} single_led_impl_t;

static void set_color(single_led_impl_t *impl, uint8_t r, uint8_t g, uint8_t b)
{
    impl->r = r;
    impl->g = g;
    impl->b = b;
}

static void turn_on(single_led_impl_t *impl)
{
    if (!impl->strip) return;
    xSemaphoreTake(impl->mutex, portMAX_DELAY);
    esp_timer_stop(impl->blink_timer);
    led_strip_set_pixel(impl->strip, 0, impl->r, impl->g, impl->b);
    led_strip_refresh(impl->strip);
    xSemaphoreGive(impl->mutex);
}

static void turn_off(single_led_impl_t *impl)
{
    if (!impl->strip) return;
    xSemaphoreTake(impl->mutex, portMAX_DELAY);
    esp_timer_stop(impl->blink_timer);
    led_strip_clear(impl->strip);
    xSemaphoreGive(impl->mutex);
}

static void blink_timer_cb(void *arg)
{
    single_led_impl_t *impl = (single_led_impl_t *)arg;
    xSemaphoreTake(impl->mutex, portMAX_DELAY);
    impl->blink_counter--;
    if (impl->blink_counter & 1) {
        led_strip_set_pixel(impl->strip, 0, impl->r, impl->g, impl->b);
        led_strip_refresh(impl->strip);
    } else {
        led_strip_clear(impl->strip);
        if (impl->blink_counter == 0) {
            esp_timer_stop(impl->blink_timer);
        }
    }
    xSemaphoreGive(impl->mutex);
}

static void start_blink(single_led_impl_t *impl, int times, int interval_ms)
{
    if (!impl->strip) return;
    xSemaphoreTake(impl->mutex, portMAX_DELAY);
    esp_timer_stop(impl->blink_timer);
    impl->blink_counter = times * 2;
    impl->blink_interval_ms = interval_ms;
    esp_timer_start_periodic(impl->blink_timer, interval_ms * 1000);
    xSemaphoreGive(impl->mutex);
}

static void start_continuous_blink(single_led_impl_t *impl, int interval_ms)
{
    start_blink(impl, BLINK_INFINITE, interval_ms);
}

static void single_led_on_state_changed(led_t *led, DeviceState state, bool is_voice_detected)
{
    single_led_impl_t *impl = (single_led_impl_t *)led;

    switch (state) {
    case kDeviceStateStarting:
        set_color(impl, 0, 0, DEFAULT_BRIGHTNESS);
        start_continuous_blink(impl, 100);
        break;
    case kDeviceStateWifiConfiguring:
        set_color(impl, 0, 0, DEFAULT_BRIGHTNESS);
        start_continuous_blink(impl, 500);
        break;
    case kDeviceStateIdle:
        turn_off(impl);
        break;
    case kDeviceStateConnecting:
        set_color(impl, 0, 0, DEFAULT_BRIGHTNESS);
        turn_on(impl);
        break;
    case kDeviceStateListening:
    case kDeviceStateAudioTesting:
        if (is_voice_detected) {
            set_color(impl, HIGH_BRIGHTNESS, 0, 0);
        } else {
            set_color(impl, LOW_BRIGHTNESS_VAL, 0, 0);
        }
        turn_on(impl);
        break;
    case kDeviceStateSpeaking:
        set_color(impl, 0, DEFAULT_BRIGHTNESS, 0);
        turn_on(impl);
        break;
    case kDeviceStateUpgrading:
        set_color(impl, 0, DEFAULT_BRIGHTNESS, 0);
        start_continuous_blink(impl, 100);
        break;
    case kDeviceStateActivating:
        set_color(impl, 0, DEFAULT_BRIGHTNESS, 0);
        start_continuous_blink(impl, 500);
        break;
    default:
        ESP_LOGW(TAG, "Unknown state: %d", state);
        break;
    }
}

static void single_led_destroy(led_t *led)
{
    single_led_impl_t *impl = (single_led_impl_t *)led;
    if (impl->blink_timer) {
        esp_timer_stop(impl->blink_timer);
        esp_timer_delete(impl->blink_timer);
    }
    if (impl->strip) {
        led_strip_del(impl->strip);
    }
    if (impl->mutex) {
        vSemaphoreDelete(impl->mutex);
    }
    free(impl);
}

static const led_ops_t single_led_ops = {
    .on_state_changed = single_led_on_state_changed,
    .destroy = single_led_destroy,
};

led_t *single_led_create(gpio_num_t gpio)
{
    if (gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "SingleLed initialized with GPIO_NUM_NC, LED will not function");
    }

    single_led_impl_t *impl = (single_led_impl_t *)calloc(1, sizeof(*impl));
    if (!impl) return NULL;

    impl->base.ops = &single_led_ops;
    impl->mutex = xSemaphoreCreateMutex();
    if (!impl->mutex) goto fail;

    if (gpio != GPIO_NUM_NC) {
        led_strip_config_t strip_config;
        memset(&strip_config, 0, sizeof(strip_config));
        strip_config.strip_gpio_num = gpio;
        strip_config.max_leds = 1;
        strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
        strip_config.led_model = LED_MODEL_WS2812;

        led_strip_rmt_config_t rmt_config;
        memset(&rmt_config, 0, sizeof(rmt_config));
        rmt_config.resolution_hz = 10 * 1000 * 1000;

        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &impl->strip));
        led_strip_clear(impl->strip);
    }

    esp_timer_create_args_t timer_args;
    memset(&timer_args, 0, sizeof(timer_args));
    timer_args.callback = blink_timer_cb;
    timer_args.arg = impl;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "blink_timer";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &impl->blink_timer));

    return &impl->base;

fail:
    if (impl->mutex) vSemaphoreDelete(impl->mutex);
    free(impl);
    return NULL;
}
