/*
 * EDA Robot Dog Movements (C port)
 * 4-legged robot dog servo movement functions.
 */
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef DEG2RAD
#define DEG2RAD(g) ((g) * M_PI / 180.0)
#endif

/* ── oscillator_t definition (must match oscillator.c) ── */
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

/* extern oscillator functions (defined in oscillator.c) */
extern unsigned long millis(void);
extern void oscillator_init(oscillator_t *osc, int trim);
extern void oscillator_attach(oscillator_t *osc, int pin, bool rev);
extern void oscillator_detach(oscillator_t *osc);
extern void oscillator_set_a(oscillator_t *osc, unsigned int amplitude);
extern void oscillator_set_o(oscillator_t *osc, int offset);
extern void oscillator_set_ph(oscillator_t *osc, double ph);
extern void oscillator_set_t(oscillator_t *osc, unsigned int T);
extern void oscillator_set_trim(oscillator_t *osc, int trim);
extern void oscillator_set_position(oscillator_t *osc, int position);
extern void oscillator_set_limiter(oscillator_t *osc, int diff_limit);
extern void oscillator_disable_limiter(oscillator_t *osc);
extern int  oscillator_get_position(oscillator_t *osc);
extern void oscillator_refresh(oscillator_t *osc);

/* ── constants ── */
#define FORWARD   1
#define BACKWARD -1
#define LEFT      1
#define RIGHT    -1

#define LEFT_FRONT_LEG  0
#define LEFT_REAR_LEG   1
#define RIGHT_FRONT_LEG 2
#define RIGHT_REAR_LEG  3
#define SERVO_COUNT     4

#define LEG_HOME_POSITION 90

static const char *TAG = "EDARobotDogMovements";

/* ── eda_robot_dog_t ── */
struct eda_robot_dog {
    oscillator_t servo[SERVO_COUNT];
    int servo_pins[SERVO_COUNT];
    int servo_trim[SERVO_COUNT];
    unsigned long final_time;
    unsigned long partial_time;
    float increment[SERVO_COUNT];
    bool is_resting;
};
typedef struct eda_robot_dog eda_robot_dog_t;

/* forward declarations */
void eda_dog_attach_servos(eda_robot_dog_t *self);
void eda_dog_move_servos(eda_robot_dog_t *self, int time, int servo_target[]);
void eda_dog_get_current_positions(eda_robot_dog_t *self, int pos[SERVO_COUNT]);

/* ── allocation / deallocation ── */

eda_robot_dog_t *eda_dog_create(void)
{
    eda_robot_dog_t *self = (eda_robot_dog_t *)calloc(1, sizeof(eda_robot_dog_t));
    if (!self) return NULL;
    for (int i = 0; i < SERVO_COUNT; i++) {
        oscillator_init(&self->servo[i], 0);
        self->servo_pins[i] = -1;
        self->servo_trim[i] = 0;
    }
    self->is_resting = false;
    return self;
}

void eda_dog_destroy(eda_robot_dog_t *self)
{
    if (!self) return;
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_detach(&self->servo[i]);
    }
    free(self);
}

/* ── Init ── */

void eda_dog_init(eda_robot_dog_t *self, int left_front_leg, int left_rear_leg,
                  int right_front_leg, int right_rear_leg)
{
    self->servo_pins[LEFT_FRONT_LEG]  = left_front_leg;
    self->servo_pins[LEFT_REAR_LEG]   = left_rear_leg;
    self->servo_pins[RIGHT_FRONT_LEG] = right_front_leg;
    self->servo_pins[RIGHT_REAR_LEG]  = right_rear_leg;

    eda_dog_attach_servos(self);
    self->is_resting = false;
}

/* ── Attach / Detach ── */

void eda_dog_attach_servos(eda_robot_dog_t *self)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_attach(&self->servo[i], self->servo_pins[i], false);
    }
}

void eda_dog_detach_servos(eda_robot_dog_t *self)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_detach(&self->servo[i]);
    }
}

/* ── Trims ── */

void eda_dog_set_trims(eda_robot_dog_t *self, int left_front_leg, int left_rear_leg,
                       int right_front_leg, int right_rear_leg)
{
    self->servo_trim[LEFT_FRONT_LEG]  = left_front_leg;
    self->servo_trim[LEFT_REAR_LEG]   = left_rear_leg;
    self->servo_trim[RIGHT_FRONT_LEG] = right_front_leg;
    self->servo_trim[RIGHT_REAR_LEG]  = right_rear_leg;

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_set_trim(&self->servo[i], self->servo_trim[i]);
    }
}

/* ── Basic motion ── */

void eda_dog_move_servos(eda_robot_dog_t *self, int time, int servo_target[])
{
    if (self->is_resting)
        self->is_resting = false;

    self->final_time = millis() + time;
    if (time > 10) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (self->servo_pins[i] != -1)
                self->increment[i] =
                    (servo_target[i] - oscillator_get_position(&self->servo[i])) / (time / 10.0f);
        }

        for (int iteration = 1; millis() < self->final_time; iteration++) {
            self->partial_time = millis() + 10;
            for (int i = 0; i < SERVO_COUNT; i++) {
                if (self->servo_pins[i] != -1)
                    oscillator_set_position(&self->servo[i],
                        oscillator_get_position(&self->servo[i]) + self->increment[i]);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (self->servo_pins[i] != -1)
                oscillator_set_position(&self->servo[i], servo_target[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(time));
    }

    bool f = true;
    int adjustment_count = 0;
    while (f && adjustment_count < 10) {
        f = false;
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (self->servo_pins[i] != -1 &&
                servo_target[i] != oscillator_get_position(&self->servo[i])) {
                f = true;
                break;
            }
        }
        if (f) {
            for (int i = 0; i < SERVO_COUNT; i++) {
                if (self->servo_pins[i] != -1)
                    oscillator_set_position(&self->servo[i], servo_target[i]);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            adjustment_count++;
        }
    }
}

void eda_dog_move_single(eda_robot_dog_t *self, int position, int servo_number)
{
    if (position > 180) position = 90;
    if (position < 0)   position = 90;

    if (self->is_resting)
        self->is_resting = false;

    if (servo_number >= 0 && servo_number < SERVO_COUNT &&
        self->servo_pins[servo_number] != -1)
        oscillator_set_position(&self->servo[servo_number], position);
}

void eda_dog_oscillate_servos(eda_robot_dog_t *self, int amplitude[SERVO_COUNT],
                              int offset[SERVO_COUNT], int period,
                              double phase_diff[SERVO_COUNT], float cycle)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1) {
            oscillator_set_o(&self->servo[i], offset[i]);
            oscillator_set_a(&self->servo[i], amplitude[i]);
            oscillator_set_t(&self->servo[i], period);
            oscillator_set_ph(&self->servo[i], phase_diff[i]);
        }
    }

    double ref = millis();
    double end_time = period * cycle + ref;

    while (millis() < end_time) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (self->servo_pins[i] != -1)
                oscillator_refresh(&self->servo[i]);
        }
        vTaskDelay(5);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void eda_dog_execute(eda_robot_dog_t *self, int amplitude[SERVO_COUNT],
                            int offset[SERVO_COUNT], int period,
                            double phase_diff[SERVO_COUNT], float steps)
{
    if (self->is_resting)
        self->is_resting = false;

    int cycles = (int)steps;

    if (cycles >= 1)
        for (int i = 0; i < cycles; i++)
            eda_dog_oscillate_servos(self, amplitude, offset, period, phase_diff, 1.0f);

    eda_dog_oscillate_servos(self, amplitude, offset, period, phase_diff, steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ── Home ── */

void eda_dog_home(eda_robot_dog_t *self)
{
    if (!self->is_resting) {
        int homes[SERVO_COUNT] = {
            LEG_HOME_POSITION, LEG_HOME_POSITION,
            LEG_HOME_POSITION, LEG_HOME_POSITION,
        };
        eda_dog_move_servos(self, 500, homes);
        self->is_resting = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

bool eda_dog_get_rest_state(eda_robot_dog_t *self) { return self->is_resting; }
void eda_dog_set_rest_state(eda_robot_dog_t *self, bool state) { self->is_resting = state; }

/* ── Helper: get current positions ── */

void eda_dog_get_current_positions(eda_robot_dog_t *self, int pos[SERVO_COUNT])
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            pos[i] = oscillator_get_position(&self->servo[i]);
        else
            pos[i] = LEG_HOME_POSITION;
    }
}

/* ── Leg lifts ── */

void eda_dog_lift_left_front_leg(eda_robot_dog_t *self, int period, int height)
{
    int current_pos[SERVO_COUNT];
    eda_dog_get_current_positions(self, current_pos);

    for (int num = 0; num < 3; num++) {
        current_pos[LEFT_FRONT_LEG] = 0;
        eda_dog_move_servos(self, 100, current_pos);
        current_pos[LEFT_FRONT_LEG] = 30;
        eda_dog_move_servos(self, 100, current_pos);
    }
    current_pos[LEFT_FRONT_LEG] = 90;
    eda_dog_move_servos(self, 100, current_pos);
}

void eda_dog_lift_left_rear_leg(eda_robot_dog_t *self, int period, int height)
{
    int current_pos[SERVO_COUNT];
    eda_dog_get_current_positions(self, current_pos);

    for (int num = 0; num < 3; num++) {
        current_pos[LEFT_REAR_LEG] = 180;
        eda_dog_move_servos(self, 100, current_pos);
        current_pos[LEFT_REAR_LEG] = 150;
        eda_dog_move_servos(self, 100, current_pos);
    }
    current_pos[LEFT_REAR_LEG] = 90;
    eda_dog_move_servos(self, 100, current_pos);
}

void eda_dog_lift_right_front_leg(eda_robot_dog_t *self, int period, int height)
{
    int current_pos[SERVO_COUNT];
    eda_dog_get_current_positions(self, current_pos);

    for (int num = 0; num < 3; num++) {
        current_pos[RIGHT_FRONT_LEG] = 180;
        eda_dog_move_servos(self, 100, current_pos);
        current_pos[RIGHT_FRONT_LEG] = 150;
        eda_dog_move_servos(self, 100, current_pos);
    }
    current_pos[RIGHT_FRONT_LEG] = 90;
    eda_dog_move_servos(self, 100, current_pos);
}

void eda_dog_lift_right_rear_leg(eda_robot_dog_t *self, int period, int height)
{
    int current_pos[SERVO_COUNT];
    eda_dog_get_current_positions(self, current_pos);

    for (int num = 0; num < 3; num++) {
        current_pos[RIGHT_REAR_LEG] = 0;
        eda_dog_move_servos(self, 100, current_pos);
        current_pos[RIGHT_REAR_LEG] = 30;
        eda_dog_move_servos(self, 100, current_pos);
    }
    current_pos[RIGHT_FRONT_LEG] = 90;
    eda_dog_move_servos(self, 100, current_pos);
}

/* ── Gait: Walk ── */

void eda_dog_walk(eda_robot_dog_t *self, float steps, int period, int dir)
{
    if (self->is_resting)
        self->is_resting = false;

    const int lift  = 25;
    const int swing = 30;
    int t = period / 6;
    if (t < 50) t = 50;

    int fwd = (dir == FORWARD) ? 1 : -1;

    for (int step = 0; step < (int)steps; step++) {
        int pos[SERVO_COUNT];
        eda_dog_get_current_positions(self, pos);

        pos[LEFT_FRONT_LEG] = 90 - lift;
        pos[RIGHT_REAR_LEG] = 90 - lift;
        eda_dog_move_servos(self, t, pos);

        pos[LEFT_FRONT_LEG]  = 90 - lift - fwd * swing;
        pos[RIGHT_REAR_LEG]  = 90 - lift + fwd * swing;
        pos[LEFT_REAR_LEG]   = 90 + fwd * swing;
        pos[RIGHT_FRONT_LEG] = 90 - fwd * swing;
        eda_dog_move_servos(self, t, pos);

        pos[LEFT_FRONT_LEG] = 90 - fwd * swing;
        pos[RIGHT_REAR_LEG] = 90 + fwd * swing;
        eda_dog_move_servos(self, t / 2, pos);

        pos[LEFT_REAR_LEG]   = 90 + lift;
        pos[RIGHT_FRONT_LEG] = 90 + lift;
        eda_dog_move_servos(self, t, pos);

        pos[LEFT_REAR_LEG]   = 90 + lift - fwd * swing;
        pos[RIGHT_FRONT_LEG] = 90 + lift + fwd * swing;
        pos[LEFT_FRONT_LEG]  = 90 + fwd * swing;
        pos[RIGHT_REAR_LEG]  = 90 - fwd * swing;
        eda_dog_move_servos(self, t, pos);

        pos[LEFT_REAR_LEG]   = 90 - fwd * swing;
        pos[RIGHT_FRONT_LEG] = 90 + fwd * swing;
        eda_dog_move_servos(self, t / 2, pos);
    }

    int home[SERVO_COUNT] = {90, 90, 90, 90};
    eda_dog_move_servos(self, 150, home);
}

/* ── Gait: Turn ── */

void eda_dog_turn(eda_robot_dog_t *self, float steps, int period, int dir)
{
    if (self->is_resting)
        self->is_resting = false;

    const int swing = 60;
    const int lift  = 25;
    int t = period / 6;
    if (t < 50) t = 50;

    int d = (dir == LEFT) ? -1 : 1;

    for (int step = 0; step < (int)steps; step++) {
        int pos[SERVO_COUNT];
        eda_dog_get_current_positions(self, pos);

        pos[LEFT_FRONT_LEG] = 90 - lift;
        pos[RIGHT_REAR_LEG] = 90 - lift;
        eda_dog_move_servos(self, t, pos);

        pos[LEFT_FRONT_LEG]  = 90 - lift + d * swing;
        pos[RIGHT_REAR_LEG]  = 90 - lift + d * swing;
        pos[LEFT_REAR_LEG]   = 90 - d * swing;
        pos[RIGHT_FRONT_LEG] = 90 - d * swing;
        eda_dog_move_servos(self, t, pos);

        pos[LEFT_FRONT_LEG] = 90 + d * swing;
        pos[RIGHT_REAR_LEG] = 90 + d * swing;
        eda_dog_move_servos(self, t / 2, pos);

        pos[LEFT_REAR_LEG]   = 90 + lift;
        pos[RIGHT_FRONT_LEG] = 90 + lift;
        eda_dog_move_servos(self, t, pos);

        pos[LEFT_REAR_LEG]   = 90 + lift + d * swing;
        pos[RIGHT_FRONT_LEG] = 90 + lift + d * swing;
        pos[LEFT_FRONT_LEG]  = 90 - d * swing;
        pos[RIGHT_REAR_LEG]  = 90 - d * swing;
        eda_dog_move_servos(self, t, pos);

        pos[LEFT_REAR_LEG]   = 90 + d * swing;
        pos[RIGHT_FRONT_LEG] = 90 + d * swing;
        eda_dog_move_servos(self, t / 2, pos);
    }

    int home[SERVO_COUNT] = {90, 90, 90, 90};
    eda_dog_move_servos(self, 150, home);
}

/* ── Postures ── */

void eda_dog_sit(eda_robot_dog_t *self, int period)
{
    int current_pos[SERVO_COUNT];
    eda_dog_get_current_positions(self, current_pos);

    current_pos[LEFT_REAR_LEG]  = 0;
    current_pos[RIGHT_REAR_LEG] = 180;
    eda_dog_move_servos(self, 100, current_pos);
}

void eda_dog_stand(eda_robot_dog_t *self, int period)
{
    eda_dog_home(self);
}

void eda_dog_stretch(eda_robot_dog_t *self, int period)
{
    int current_pos[SERVO_COUNT];
    eda_dog_get_current_positions(self, current_pos);

    current_pos[LEFT_FRONT_LEG]  = 0;
    current_pos[RIGHT_REAR_LEG]  = 0;
    current_pos[LEFT_REAR_LEG]   = 180;
    current_pos[RIGHT_FRONT_LEG] = 180;
    eda_dog_move_servos(self, 100, current_pos);
}

void eda_dog_shake(eda_robot_dog_t *self, int period)
{
    int A[SERVO_COUNT] = {20, 0, 20, 0};
    int O[SERVO_COUNT] = {0, LEG_HOME_POSITION, 0, LEG_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(180), 0, DEG2RAD(0), 0};

    eda_dog_execute(self, A, O, period, phase_diff, 3.0f);
}

void eda_dog_enable_servo_limit(eda_robot_dog_t *self, int diff_limit)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_set_limiter(&self->servo[i], diff_limit);
    }
}

void eda_dog_disable_servo_limit(eda_robot_dog_t *self)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_disable_limiter(&self->servo[i]);
    }
}

void eda_dog_sleep(eda_robot_dog_t *self)
{
    int current_pos[SERVO_COUNT];
    eda_dog_get_current_positions(self, current_pos);

    current_pos[LEFT_FRONT_LEG]  = 0;
    current_pos[RIGHT_REAR_LEG]  = 180;
    current_pos[LEFT_REAR_LEG]   = 180;
    current_pos[RIGHT_FRONT_LEG] = 0;
    eda_dog_move_servos(self, 100, current_pos);
}
