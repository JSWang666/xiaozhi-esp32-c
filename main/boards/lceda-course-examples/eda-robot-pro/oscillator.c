/*
 * Oscillator - Generate sinusoidal oscillations in servos (C port)
 * (c) Juan Gonzalez-Gomez (Obijuan), Dec 2011
 * (c) txp666 for esp32, 202503
 * GPL license
 */
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define SERVO_MIN_PULSEWIDTH_US 500
#define SERVO_MAX_PULSEWIDTH_US 2500
#define SERVO_MIN_DEGREE        -90
#define SERVO_MAX_DEGREE         90

static const char *TAG = "Oscillator";

typedef struct oscillator {
    bool is_attached;
    unsigned int amplitude;
    int offset;
    unsigned int period;
    double phase0;
    int pos;
    int pin;
    int trim;
    double phase;
    double inc;
    double number_samples;
    unsigned int sampling_period;
    long previous_millis;
    long current_millis;
    bool stop;
    bool rev;
    int diff_limit;
    long previous_servo_command_millis;
    ledc_channel_t ledc_channel;
    ledc_mode_t ledc_speed_mode;
} oscillator_t;

unsigned long IRAM_ATTR millis(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

void oscillator_init(oscillator_t *osc, int trim)
{
    osc->trim = trim;
    osc->diff_limit = 0;
    osc->is_attached = false;
    osc->sampling_period = 30;
    osc->period = 2000;
    osc->number_samples = osc->period / osc->sampling_period;
    osc->inc = 2.0 * M_PI / osc->number_samples;
    osc->amplitude = 45;
    osc->phase = 0;
    osc->phase0 = 0;
    osc->offset = 0;
    osc->stop = false;
    osc->rev = false;
    osc->pos = 90;
    osc->previous_millis = 0;
}

static bool oscillator_next_sample(oscillator_t *osc)
{
    osc->current_millis = millis();
    if (osc->current_millis - osc->previous_millis > (long)osc->sampling_period) {
        osc->previous_millis = osc->current_millis;
        return true;
    }
    return false;
}

static void oscillator_write(oscillator_t *osc, int position)
{
    if (!osc->is_attached)
        return;

    long current_ms = millis();
    if (osc->diff_limit > 0) {
        int limit = MAX(1,
            (((int)(current_ms - osc->previous_servo_command_millis)) * osc->diff_limit) / 1000);
        if (abs(position - osc->pos) > limit) {
            osc->pos += position < osc->pos ? -limit : limit;
        } else {
            osc->pos = position;
        }
    } else {
        osc->pos = position;
    }
    osc->previous_servo_command_millis = current_ms;

    int angle = osc->pos + osc->trim;
    angle = MIN(MAX(angle, 0), 180);

    uint32_t duty = (uint32_t)(((angle / 180.0) * 2.0 + 0.5) * 8191 / 20.0);
    ESP_ERROR_CHECK(ledc_set_duty(osc->ledc_speed_mode, osc->ledc_channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(osc->ledc_speed_mode, osc->ledc_channel));
}

void oscillator_detach(oscillator_t *osc)
{
    if (!osc->is_attached)
        return;
    ESP_ERROR_CHECK(ledc_stop(osc->ledc_speed_mode, osc->ledc_channel, 0));
    osc->is_attached = false;
}

void oscillator_attach(oscillator_t *osc, int pin, bool rev)
{
    if (osc->is_attached)
        oscillator_detach(osc);

    osc->pin = pin;
    osc->rev = rev;

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
    osc->ledc_channel = (ledc_channel_t)last_channel;

    ledc_channel_config_t ch_cfg = {
        .gpio_num = osc->pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = osc->ledc_channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    osc->ledc_speed_mode = LEDC_LOW_SPEED_MODE;
    osc->previous_servo_command_millis = millis();
    osc->is_attached = true;
}

void oscillator_set_t(oscillator_t *osc, unsigned int T)
{
    osc->period = T;
    osc->number_samples = osc->period / osc->sampling_period;
    osc->inc = 2.0 * M_PI / osc->number_samples;
}

void oscillator_set_position(oscillator_t *osc, int position)
{
    oscillator_write(osc, position);
}

void oscillator_refresh(oscillator_t *osc)
{
    if (oscillator_next_sample(osc)) {
        if (!osc->stop) {
            int pos = (int)round(osc->amplitude * sin(osc->phase + osc->phase0) + osc->offset);
            if (osc->rev)
                pos = -pos;
            oscillator_write(osc, pos + 90);
        }
        osc->phase += osc->inc;
    }
}

void oscillator_set_a(oscillator_t *osc, unsigned int amplitude) { osc->amplitude = amplitude; }
void oscillator_set_o(oscillator_t *osc, int offset)             { osc->offset = offset; }
void oscillator_set_ph(oscillator_t *osc, double ph)             { osc->phase0 = ph; }
void oscillator_set_trim(oscillator_t *osc, int trim)            { osc->trim = trim; }
void oscillator_set_limiter(oscillator_t *osc, int diff_limit)   { osc->diff_limit = diff_limit; }
void oscillator_disable_limiter(oscillator_t *osc)               { osc->diff_limit = 0; }
int  oscillator_get_trim(oscillator_t *osc)                      { return osc->trim; }
int  oscillator_get_position(oscillator_t *osc)                  { return osc->pos; }
void oscillator_stop_osc(oscillator_t *osc)                      { osc->stop = true; }
void oscillator_play(oscillator_t *osc)                          { osc->stop = false; }
void oscillator_reset_phase(oscillator_t *osc)                   { osc->phase = 0; }
