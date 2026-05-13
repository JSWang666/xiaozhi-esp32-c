/*
 * Oscillator - Generate sinusoidal oscillations in the servos
 * (c) Juan Gonzalez-Gomez (Obijuan), Dec 2011
 * (c) txp666 for esp32, 202503
 * GPL license
 *
 * Converted from C++ to C.
 */

#include <driver/ledc.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

#define TAG "Oscillator"

#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef DEG2RAD
#define DEG2RAD(g) ((g) * M_PI / 180.0)
#endif

#define SERVO_MIN_PULSEWIDTH_US 500
#define SERVO_MAX_PULSEWIDTH_US 2500
#define SERVO_MIN_DEGREE (-90)
#define SERVO_MAX_DEGREE 90

typedef struct oscillator_s {
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

extern unsigned long millis(void);

/* forward declarations */
static void oscillator_write(oscillator_t *self, int position);
static bool oscillator_next_sample(oscillator_t *self);

void oscillator_init(oscillator_t *self, int trim) {
    self->trim = trim;
    self->diff_limit = 0;
    self->is_attached = false;
    self->sampling_period = 30;
    self->period = 2000;
    self->number_samples = self->period / self->sampling_period;
    self->inc = 2.0 * M_PI / self->number_samples;
    self->amplitude = 45;
    self->phase = 0;
    self->phase0 = 0;
    self->offset = 0;
    self->stop = false;
    self->rev = false;
    self->pos = 90;
    self->previous_millis = 0;
    self->previous_servo_command_millis = 0;
    self->ledc_channel = LEDC_CHANNEL_0;
    self->ledc_speed_mode = LEDC_LOW_SPEED_MODE;
}

void oscillator_detach(oscillator_t *self) {
    if (!self->is_attached) return;
    ESP_ERROR_CHECK(ledc_stop(self->ledc_speed_mode, self->ledc_channel, 0));
    self->is_attached = false;
}

void oscillator_attach(oscillator_t *self, int pin, bool rev) {
    if (self->is_attached) {
        oscillator_detach(self);
    }

    self->pin = pin;
    self->rev = rev;

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    static int last_channel = 0;
    last_channel = (last_channel + 1) % 7 + 1;
    self->ledc_channel = (ledc_channel_t)last_channel;

    ledc_channel_config_t ch_cfg = {
        .gpio_num = self->pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = self->ledc_channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    self->ledc_speed_mode = LEDC_LOW_SPEED_MODE;
    self->previous_servo_command_millis = millis();
    self->is_attached = true;
}

void oscillator_set_a(oscillator_t *self, unsigned int amplitude) {
    self->amplitude = amplitude;
}

void oscillator_set_o(oscillator_t *self, int offset) {
    self->offset = offset;
}

void oscillator_set_ph(oscillator_t *self, double ph) {
    self->phase0 = ph;
}

void oscillator_set_t(oscillator_t *self, unsigned int period) {
    self->period = period;
    self->number_samples = self->period / self->sampling_period;
    self->inc = 2.0 * M_PI / self->number_samples;
}

void oscillator_set_trim(oscillator_t *self, int trim) {
    self->trim = trim;
}

int oscillator_get_trim(oscillator_t *self) {
    return self->trim;
}

void oscillator_set_position(oscillator_t *self, int position) {
    oscillator_write(self, position);
}

void oscillator_set_limiter(oscillator_t *self, int diff_limit) {
    self->diff_limit = diff_limit;
}

void oscillator_disable_limiter(oscillator_t *self) {
    self->diff_limit = 0;
}

void oscillator_stop_osc(oscillator_t *self) {
    self->stop = true;
}

void oscillator_play(oscillator_t *self) {
    self->stop = false;
}

void oscillator_reset(oscillator_t *self) {
    self->phase = 0;
}

int oscillator_get_position(oscillator_t *self) {
    return self->pos;
}

void oscillator_refresh(oscillator_t *self) {
    if (oscillator_next_sample(self)) {
        if (!self->stop) {
            int p = (int)round(self->amplitude * sin(self->phase + self->phase0) + self->offset);
            if (self->rev) p = -p;
            oscillator_write(self, p + 90);
        }
        self->phase += self->inc;
    }
}

static bool oscillator_next_sample(oscillator_t *self) {
    self->current_millis = millis();
    if (self->current_millis - self->previous_millis > self->sampling_period) {
        self->previous_millis = self->current_millis;
        return true;
    }
    return false;
}

static void oscillator_write(oscillator_t *self, int position) {
    if (!self->is_attached) return;

    long cur = millis();
    if (self->diff_limit > 0) {
        int limit = MAX(1, (((int)(cur - self->previous_servo_command_millis)) * self->diff_limit) / 1000);
        if (abs(position - self->pos) > limit) {
            self->pos += position < self->pos ? -limit : limit;
        } else {
            self->pos = position;
        }
    } else {
        self->pos = position;
    }
    self->previous_servo_command_millis = cur;

    int angle = self->pos + self->trim;
    angle = MIN(MAX(angle, 0), 180);

    uint32_t duty = (uint32_t)(((angle / 180.0) * 2.0 + 0.5) * 8191 / 20.0);
    ESP_ERROR_CHECK(ledc_set_duty(self->ledc_speed_mode, self->ledc_channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(self->ledc_speed_mode, self->ledc_channel));
}
