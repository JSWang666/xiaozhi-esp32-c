//--------------------------------------------------------------
//-- Oscillator.pde
//-- Generate sinusoidal oscillations in the servos
//--------------------------------------------------------------
//-- (c) Juan Gonzalez-Gomez (Obijuan), Dec 2011
//-- (c) txp666 for esp32, 202503
//-- GPL license
//--------------------------------------------------------------
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define M_PI 3.14159265358979323846

#define SERVO_MIN_PULSEWIDTH_US 500
#define SERVO_MAX_PULSEWIDTH_US 2500
#define SERVO_MIN_DEGREE -90
#define SERVO_MAX_DEGREE 90

static const char *TAG = "Oscillator";

static unsigned long IRAM_ATTR millis(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

typedef struct {
    bool is_attached;
    unsigned int amplitude;
    int offset;
    unsigned int period;
    double phase0;
    int pos, pin, trim;
    double phase, inc, number_samples;
    unsigned int sampling_period;
    long previous_millis, current_millis;
    bool stop, rev;
    int diff_limit;
    long previous_servo_command_millis;
    ledc_channel_t ledc_channel;
    ledc_mode_t ledc_speed_mode;
} oscillator_t;

#define MAX_VAL(a, b) ((a) > (b) ? (a) : (b))
#define MIN_VAL(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(v, lo, hi) (MIN_VAL(MAX_VAL((v), (lo)), (hi)))

static void oscillator_write(oscillator_t *o, int position);

void oscillator_init(oscillator_t *o, int trim)
{
    memset(o, 0, sizeof(*o));
    o->trim = trim;
    o->diff_limit = 0;
    o->is_attached = false;
    o->sampling_period = 30;
    o->period = 2000;
    o->number_samples = o->period / o->sampling_period;
    o->inc = 2.0 * M_PI / o->number_samples;
    o->amplitude = 45;
    o->phase = 0;
    o->phase0 = 0;
    o->offset = 0;
    o->stop = false;
    o->rev = false;
    o->pos = 90;
    o->previous_millis = 0;
}

void oscillator_detach(oscillator_t *o)
{
    if (!o->is_attached)
        return;
    ESP_ERROR_CHECK(ledc_stop(o->ledc_speed_mode, o->ledc_channel, 0));
    o->is_attached = false;
}

static bool oscillator_next_sample(oscillator_t *o)
{
    o->current_millis = millis();
    if ((unsigned long)(o->current_millis - o->previous_millis) > o->sampling_period) {
        o->previous_millis = o->current_millis;
        return true;
    }
    return false;
}

void oscillator_attach(oscillator_t *o, int pin, bool rev)
{
    if (o->is_attached)
        oscillator_detach(o);

    o->pin = pin;
    o->rev = rev;

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    static int last_channel = 0;
    last_channel = (last_channel + 1) % 7 + 1;
    o->ledc_channel = (ledc_channel_t)last_channel;

    ledc_channel_config_t ledc_ch = {
        .gpio_num = o->pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = o->ledc_channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch));

    o->ledc_speed_mode = LEDC_LOW_SPEED_MODE;
    o->previous_servo_command_millis = millis();
    o->is_attached = true;
}

void oscillator_set_t(oscillator_t *o, unsigned int T)
{
    o->period = T;
    o->number_samples = o->period / o->sampling_period;
    o->inc = 2.0 * M_PI / o->number_samples;
}

void oscillator_set_position(oscillator_t *o, int position)
{
    oscillator_write(o, position);
}

void oscillator_refresh(oscillator_t *o)
{
    if (oscillator_next_sample(o)) {
        if (!o->stop) {
            int p = (int)round(o->amplitude * sin(o->phase + o->phase0) + o->offset);
            if (o->rev)
                p = -p;
            oscillator_write(o, p + 90);
        }
        o->phase += o->inc;
    }
}

static void oscillator_write(oscillator_t *o, int position)
{
    if (!o->is_attached)
        return;

    long cur = millis();
    if (o->diff_limit > 0) {
        int limit = MAX_VAL(1, (((int)(cur - o->previous_servo_command_millis)) * o->diff_limit) / 1000);
        if (abs(position - o->pos) > limit)
            o->pos += position < o->pos ? -limit : limit;
        else
            o->pos = position;
    } else {
        o->pos = position;
    }
    o->previous_servo_command_millis = cur;

    int angle = CLAMP(o->pos + o->trim, 0, 180);
    uint32_t duty = (uint32_t)(((angle / 180.0) * 2.0 + 0.5) * 8191 / 20.0);

    ESP_ERROR_CHECK(ledc_set_duty(o->ledc_speed_mode, o->ledc_channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(o->ledc_speed_mode, o->ledc_channel));
}

void oscillator_set_a(oscillator_t *o, unsigned int amplitude) { o->amplitude = amplitude; }
void oscillator_set_o(oscillator_t *o, int offset) { o->offset = offset; }
void oscillator_set_ph(oscillator_t *o, double ph) { o->phase0 = ph; }
void oscillator_set_trim(oscillator_t *o, int trim) { o->trim = trim; }
void oscillator_set_limiter(oscillator_t *o, int diff_limit) { o->diff_limit = diff_limit; }
void oscillator_disable_limiter(oscillator_t *o) { o->diff_limit = 0; }
int  oscillator_get_trim(oscillator_t *o) { return o->trim; }
int  oscillator_get_position(oscillator_t *o) { return o->pos; }
void oscillator_stop(oscillator_t *o) { o->stop = true; }
void oscillator_play(oscillator_t *o) { o->stop = false; }
void oscillator_reset(oscillator_t *o) { o->phase = 0; }
