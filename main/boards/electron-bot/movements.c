#include <driver/ledc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FORWARD 1
#define BACKWARD (-1)
#define LEFT 1
#define RIGHT (-1)
#define BOTH 0
#define SMALL 5
#define MEDIUM 15
#define BIG 30

#define SERVO_LIMIT_DEFAULT 240

#define RIGHT_PITCH 0
#define RIGHT_ROLL 1
#define LEFT_PITCH 2
#define LEFT_ROLL 3
#define BODY 4
#define HEAD 5
#define SERVO_COUNT 6

#define MOV_MAX(a, b) ((a) > (b) ? (a) : (b))
#define MOV_MIN(a, b) ((a) < (b) ? (a) : (b))

/* ---------- oscillator_t (must match oscillator.c definition) ---------- */
typedef struct {
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

extern void oscillator_init(oscillator_t *osc, int trim);
extern void oscillator_attach(oscillator_t *osc, int pin, bool rev);
extern void oscillator_detach(oscillator_t *osc);
extern void oscillator_set_a(oscillator_t *osc, unsigned int amplitude);
extern void oscillator_set_o(oscillator_t *osc, int offset);
extern void oscillator_set_ph(oscillator_t *osc, double ph);
extern void oscillator_set_t(oscillator_t *osc, unsigned int period);
extern void oscillator_set_trim(oscillator_t *osc, int trim);
extern void oscillator_set_position(oscillator_t *osc, int position);
extern int  oscillator_get_position(oscillator_t *osc);
extern void oscillator_refresh(oscillator_t *osc);

/* ---------- otto_t ---------- */
typedef struct {
    oscillator_t servo[SERVO_COUNT];
    int servo_pins[SERVO_COUNT];
    int servo_trim[SERVO_COUNT];
    int servo_initial[SERVO_COUNT];
    unsigned long final_time;
    unsigned long partial_time;
    float increment[SERVO_COUNT];
    bool is_resting;
} otto_t;

/* forward declarations */
void otto_attach_servos(otto_t *o);
void otto_move_servos(otto_t *o, int time, int servo_target[]);

/* millis() — the single definition used by oscillator.c as well */
unsigned long IRAM_ATTR millis(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

void otto_init(otto_t *o, int right_pitch, int right_roll,
               int left_pitch, int left_roll, int body, int head)
{
    o->is_resting = false;
    int defaults[SERVO_COUNT] = {180, 180, 0, 0, 90, 90};
    memcpy(o->servo_initial, defaults, sizeof(defaults));

    for (int i = 0; i < SERVO_COUNT; i++) {
        oscillator_init(&o->servo[i], 0);
        o->servo_pins[i] = -1;
        o->servo_trim[i] = 0;
    }

    o->servo_pins[RIGHT_PITCH] = right_pitch;
    o->servo_pins[RIGHT_ROLL]  = right_roll;
    o->servo_pins[LEFT_PITCH]  = left_pitch;
    o->servo_pins[LEFT_ROLL]   = left_roll;
    o->servo_pins[BODY]        = body;
    o->servo_pins[HEAD]        = head;

    otto_attach_servos(o);
    o->is_resting = false;
}

void otto_attach_servos(otto_t *o)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (o->servo_pins[i] != -1)
            oscillator_attach(&o->servo[i], o->servo_pins[i], false);
    }
}

void otto_detach_servos(otto_t *o)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (o->servo_pins[i] != -1)
            oscillator_detach(&o->servo[i]);
    }
}

void otto_set_trims(otto_t *o, int right_pitch, int right_roll,
                    int left_pitch, int left_roll, int body, int head)
{
    o->servo_trim[RIGHT_PITCH] = right_pitch;
    o->servo_trim[RIGHT_ROLL]  = right_roll;
    o->servo_trim[LEFT_PITCH]  = left_pitch;
    o->servo_trim[LEFT_ROLL]   = left_roll;
    o->servo_trim[BODY]        = body;
    o->servo_trim[HEAD]        = head;

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (o->servo_pins[i] != -1)
            oscillator_set_trim(&o->servo[i], o->servo_trim[i]);
    }
}

void otto_move_servos(otto_t *o, int time, int servo_target[])
{
    if (o->is_resting)
        o->is_resting = false;

    o->final_time = millis() + time;
    if (time > 10) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (o->servo_pins[i] != -1)
                o->increment[i] = (servo_target[i] - oscillator_get_position(&o->servo[i])) / (time / 10.0f);
        }

        for (int iteration = 1; millis() < o->final_time; iteration++) {
            o->partial_time = millis() + 10;
            for (int i = 0; i < SERVO_COUNT; i++) {
                if (o->servo_pins[i] != -1)
                    oscillator_set_position(&o->servo[i],
                        oscillator_get_position(&o->servo[i]) + (int)o->increment[i]);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (o->servo_pins[i] != -1)
                oscillator_set_position(&o->servo[i], servo_target[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(time));
    }

    bool f = true;
    int adjustment_count = 0;
    while (f && adjustment_count < 10) {
        f = false;
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (o->servo_pins[i] != -1 &&
                servo_target[i] != oscillator_get_position(&o->servo[i])) {
                f = true;
                break;
            }
        }
        if (f) {
            for (int i = 0; i < SERVO_COUNT; i++) {
                if (o->servo_pins[i] != -1)
                    oscillator_set_position(&o->servo[i], servo_target[i]);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            adjustment_count++;
        }
    }
}

void otto_move_single(otto_t *o, int position, int servo_number)
{
    if (position > 180) position = 90;
    if (position < 0) position = 90;

    if (o->is_resting)
        o->is_resting = false;

    if (servo_number >= 0 && servo_number < SERVO_COUNT &&
        o->servo_pins[servo_number] != -1) {
        oscillator_set_position(&o->servo[servo_number], position);
    }
}

static void otto_oscillate_servos(otto_t *o, int amplitude[SERVO_COUNT],
                                  int offset[SERVO_COUNT], int period,
                                  double phase_diff[SERVO_COUNT], float cycle)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (o->servo_pins[i] != -1) {
            oscillator_set_o(&o->servo[i], offset[i]);
            oscillator_set_a(&o->servo[i], amplitude[i]);
            oscillator_set_t(&o->servo[i], period);
            oscillator_set_ph(&o->servo[i], phase_diff[i]);
        }
    }

    double ref = millis();
    double end_time = period * cycle + ref;

    while (millis() < end_time) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (o->servo_pins[i] != -1)
                oscillator_refresh(&o->servo[i]);
        }
        vTaskDelay(5);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void otto_execute(otto_t *o, int amplitude[SERVO_COUNT],
                         int offset[SERVO_COUNT], int period,
                         double phase_diff[SERVO_COUNT], float steps)
{
    if (o->is_resting)
        o->is_resting = false;

    int cycles = (int)steps;
    if (cycles >= 1) {
        for (int i = 0; i < cycles; i++)
            otto_oscillate_servos(o, amplitude, offset, period, phase_diff, 1.0f);
    }

    otto_oscillate_servos(o, amplitude, offset, period, phase_diff, steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void otto_home(otto_t *o, bool hands_down)
{
    (void)hands_down;
    if (!o->is_resting) {
        otto_move_servos(o, 1000, o->servo_initial);
        o->is_resting = true;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

bool otto_get_rest_state(otto_t *o)
{
    return o->is_resting;
}

void otto_set_rest_state(otto_t *o, bool state)
{
    o->is_resting = state;
}

void otto_hand_action(otto_t *o, int action, int times, int amount, int period)
{
    times  = 2 * MOV_MAX(3, MOV_MIN(100, times));
    amount = MOV_MAX(10, MOV_MIN(50, amount));
    period = MOV_MAX(100, MOV_MIN(1000, period));

    int cp[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++)
        cp[i] = (o->servo_pins[i] != -1) ? oscillator_get_position(&o->servo[i]) : o->servo_initial[i];

    switch (action) {
    case 1:
        cp[LEFT_PITCH] = 180;
        otto_move_servos(o, period, cp);
        break;
    case 2:
        cp[RIGHT_PITCH] = 0;
        otto_move_servos(o, period, cp);
        break;
    case 3:
        cp[LEFT_PITCH] = 180;
        cp[RIGHT_PITCH] = 0;
        otto_move_servos(o, period, cp);
        break;
    case 4:
    case 5:
    case 6:
        memcpy(cp, o->servo_initial, sizeof(cp));
        otto_move_servos(o, period, cp);
        break;
    case 7:
        cp[LEFT_PITCH] = 150;
        otto_move_servos(o, period, cp);
        for (int i = 0; i < times; i++) {
            cp[LEFT_PITCH] = 150 + (i % 2 == 0 ? -30 : 30);
            otto_move_servos(o, period / 10, cp);
            vTaskDelay(pdMS_TO_TICKS(period / 10));
        }
        memcpy(cp, o->servo_initial, sizeof(cp));
        otto_move_servos(o, period, cp);
        break;
    case 8:
        cp[RIGHT_PITCH] = 30;
        otto_move_servos(o, period, cp);
        for (int i = 0; i < times; i++) {
            cp[RIGHT_PITCH] = 30 + (i % 2 == 0 ? 30 : -30);
            otto_move_servos(o, period / 10, cp);
            vTaskDelay(pdMS_TO_TICKS(period / 10));
        }
        memcpy(cp, o->servo_initial, sizeof(cp));
        otto_move_servos(o, period, cp);
        break;
    case 9:
        cp[LEFT_PITCH] = 150;
        cp[RIGHT_PITCH] = 30;
        otto_move_servos(o, period, cp);
        for (int i = 0; i < times; i++) {
            cp[LEFT_PITCH]  = 150 + (i % 2 == 0 ? -30 : 30);
            cp[RIGHT_PITCH] = 30  + (i % 2 == 0 ?  30 : -30);
            otto_move_servos(o, period / 10, cp);
            vTaskDelay(pdMS_TO_TICKS(period / 10));
        }
        memcpy(cp, o->servo_initial, sizeof(cp));
        otto_move_servos(o, period, cp);
        break;
    case 10:
        cp[LEFT_ROLL] = 20;
        otto_move_servos(o, period, cp);
        for (int i = 0; i < times; i++) {
            cp[LEFT_ROLL] = 20 - amount;
            otto_move_servos(o, period / 10, cp);
            cp[LEFT_ROLL] = 20 + amount;
            otto_move_servos(o, period / 10, cp);
        }
        cp[LEFT_ROLL] = 0;
        otto_move_servos(o, period, cp);
        break;
    case 11:
        cp[RIGHT_ROLL] = 160;
        otto_move_servos(o, period, cp);
        for (int i = 0; i < times; i++) {
            cp[RIGHT_ROLL] = 160 + amount;
            otto_move_servos(o, period / 10, cp);
            cp[RIGHT_ROLL] = 160 - amount;
            otto_move_servos(o, period / 10, cp);
        }
        cp[RIGHT_ROLL] = 180;
        otto_move_servos(o, period, cp);
        break;
    case 12:
        cp[LEFT_ROLL]  = 20;
        cp[RIGHT_ROLL] = 160;
        otto_move_servos(o, period, cp);
        for (int i = 0; i < times; i++) {
            cp[LEFT_ROLL]  = 20  - amount;
            cp[RIGHT_ROLL] = 160 + amount;
            otto_move_servos(o, period / 10, cp);
            cp[LEFT_ROLL]  = 20  + amount;
            cp[RIGHT_ROLL] = 160 - amount;
            otto_move_servos(o, period / 10, cp);
        }
        cp[LEFT_ROLL]  = 0;
        cp[RIGHT_ROLL] = 180;
        otto_move_servos(o, period, cp);
        break;
    }
}

void otto_body_action(otto_t *o, int action, int times, int amount, int period)
{
    times  = MOV_MAX(1, MOV_MIN(10, times));
    amount = MOV_MAX(0, MOV_MIN(90, amount));
    period = MOV_MAX(500, MOV_MIN(3000, period));

    int cp[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++) {
        cp[i] = (o->servo_pins[i] != -1)
            ? oscillator_get_position(&o->servo[i])
            : o->servo_initial[i];
    }

    int body_center  = o->servo_initial[BODY];
    int target_angle = body_center;

    switch (action) {
    case 1:
        target_angle = MOV_MIN(180, body_center + amount);
        break;
    case 2:
        target_angle = MOV_MAX(0, body_center - amount);
        break;
    case 3:
        target_angle = body_center;
        break;
    default:
        return;
    }

    cp[BODY] = target_angle;
    otto_move_servos(o, period, cp);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void otto_head_action(otto_t *o, int action, int times, int amount, int period)
{
    times  = MOV_MAX(1, MOV_MIN(10, times));
    amount = MOV_MAX(1, MOV_MIN(15, abs(amount)));
    period = MOV_MAX(300, MOV_MIN(3000, period));

    int cp[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++) {
        cp[i] = (o->servo_pins[i] != -1)
            ? oscillator_get_position(&o->servo[i])
            : o->servo_initial[i];
    }

    int head_center = 90;

    switch (action) {
    case 1:
        cp[HEAD] = head_center + amount;
        otto_move_servos(o, period, cp);
        break;
    case 2:
        cp[HEAD] = head_center - amount;
        otto_move_servos(o, period, cp);
        break;
    case 3:
        cp[HEAD] = head_center + amount;
        otto_move_servos(o, period / 3, cp);
        vTaskDelay(pdMS_TO_TICKS(period / 6));
        cp[HEAD] = head_center - amount;
        otto_move_servos(o, period / 3, cp);
        vTaskDelay(pdMS_TO_TICKS(period / 6));
        cp[HEAD] = head_center;
        otto_move_servos(o, period / 3, cp);
        break;
    case 4:
        cp[HEAD] = head_center;
        otto_move_servos(o, period, cp);
        break;
    case 5:
        for (int i = 0; i < times; i++) {
            cp[HEAD] = head_center + amount;
            otto_move_servos(o, period / 2, cp);
            cp[HEAD] = head_center - amount;
            otto_move_servos(o, period / 2, cp);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        cp[HEAD] = head_center;
        otto_move_servos(o, period / 2, cp);
        break;
    default:
        cp[HEAD] = head_center;
        otto_move_servos(o, period, cp);
        break;
    }
}
