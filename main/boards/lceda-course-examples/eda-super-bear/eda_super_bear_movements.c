#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oscillator.c"

static const char *TAG_MOV = "EdaSuperBearMovements";

#define LEFT_LEG   0
#define RIGHT_LEG  1
#define LEFT_FOOT  2
#define RIGHT_FOOT 3
#define LEFT_HAND  4
#define RIGHT_HAND 5
#define SERVO_COUNT 6

#define FORWARD  1
#define BACKWARD -1
#define LEFT  1
#define RIGHT -1
#define BOTH  0

#define SMALL  5
#define MEDIUM 15
#define BIG    30
#define SERVO_LIMIT_DEFAULT 240

#ifndef DEG2RAD
#define DEG2RAD(g) ((g) * M_PI / 180.0)
#endif

#define HAND_HOME_POSITION 45

typedef struct {
    oscillator_t servo[SERVO_COUNT];
    int servo_pins[SERVO_COUNT];
    int servo_trim[SERVO_COUNT];
    unsigned long final_time, partial_time;
    float increment[SERVO_COUNT];
    bool is_resting, has_hands;
} eda_robot_t;

/* --------------------------------------------------------- */

void eda_robot_init_struct(eda_robot_t *self)
{
    memset(self, 0, sizeof(*self));
    self->is_resting = false;
    self->has_hands = false;
    for (int i = 0; i < SERVO_COUNT; i++) {
        self->servo_pins[i] = -1;
        self->servo_trim[i] = 0;
        oscillator_init(&self->servo[i], 0);
    }
}

void eda_robot_detach_servos(eda_robot_t *self)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_detach(&self->servo[i]);
    }
}

void eda_robot_attach_servos(eda_robot_t *self)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_attach(&self->servo[i], self->servo_pins[i], false);
    }
}

void eda_robot_init(eda_robot_t *self, int left_leg, int right_leg,
                    int left_foot, int right_foot, int left_hand, int right_hand)
{
    self->servo_pins[LEFT_LEG]   = left_leg;
    self->servo_pins[RIGHT_LEG]  = right_leg;
    self->servo_pins[LEFT_FOOT]  = left_foot;
    self->servo_pins[RIGHT_FOOT] = right_foot;
    self->servo_pins[LEFT_HAND]  = left_hand;
    self->servo_pins[RIGHT_HAND] = right_hand;
    self->has_hands = (left_hand != -1 && right_hand != -1);
    eda_robot_attach_servos(self);
    self->is_resting = false;
}

void eda_robot_set_trims(eda_robot_t *self, int left_leg, int right_leg,
                         int left_foot, int right_foot, int left_hand, int right_hand)
{
    self->servo_trim[LEFT_LEG]   = left_leg;
    self->servo_trim[RIGHT_LEG]  = right_leg;
    self->servo_trim[LEFT_FOOT]  = left_foot;
    self->servo_trim[RIGHT_FOOT] = right_foot;
    if (self->has_hands) {
        self->servo_trim[LEFT_HAND]  = left_hand;
        self->servo_trim[RIGHT_HAND] = right_hand;
    }
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_set_trim(&self->servo[i], self->servo_trim[i]);
    }
}

bool eda_robot_get_rest_state(eda_robot_t *self) { return self->is_resting; }
void eda_robot_set_rest_state(eda_robot_t *self, bool state) { self->is_resting = state; }

void eda_robot_move_servos(eda_robot_t *self, int time, int servo_target[])
{
    if (eda_robot_get_rest_state(self))
        eda_robot_set_rest_state(self, false);

    self->final_time = millis() + time;
    if (time > 10) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (self->servo_pins[i] != -1)
                self->increment[i] = (servo_target[i] - oscillator_get_position(&self->servo[i])) / (time / 10.0f);
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
            if (self->servo_pins[i] != -1 && servo_target[i] != oscillator_get_position(&self->servo[i])) {
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

void eda_robot_move_single(eda_robot_t *self, int position, int servo_number)
{
    if (position > 180) position = 90;
    if (position < 0) position = 90;
    if (eda_robot_get_rest_state(self))
        eda_robot_set_rest_state(self, false);
    if (servo_number >= 0 && servo_number < SERVO_COUNT && self->servo_pins[servo_number] != -1)
        oscillator_set_position(&self->servo[servo_number], position);
}

void eda_robot_oscillate_servos(eda_robot_t *self, int amplitude[], int offset[],
                                int period, double phase_diff[], float cycle)
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

void eda_robot_execute(eda_robot_t *self, int amplitude[], int offset[],
                       int period, double phase_diff[], float steps)
{
    if (eda_robot_get_rest_state(self))
        eda_robot_set_rest_state(self, false);

    int cycles = (int)steps;
    if (cycles >= 1) {
        for (int i = 0; i < cycles; i++)
            eda_robot_oscillate_servos(self, amplitude, offset, period, phase_diff, 1.0f);
    }
    eda_robot_oscillate_servos(self, amplitude, offset, period, phase_diff, steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void eda_robot_home(eda_robot_t *self, bool hands_down)
{
    if (!self->is_resting) {
        int homes[SERVO_COUNT];
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (i == LEFT_HAND || i == RIGHT_HAND) {
                if (hands_down) {
                    homes[i] = (i == LEFT_HAND) ? HAND_HOME_POSITION : (180 - HAND_HOME_POSITION);
                } else {
                    homes[i] = oscillator_get_position(&self->servo[i]);
                }
            } else {
                homes[i] = 90;
            }
        }
        eda_robot_move_servos(self, 500, homes);
        self->is_resting = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

void eda_robot_jump(eda_robot_t *self, float steps, int period)
{
    int up[SERVO_COUNT]   = {90, 90, 150, 30, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    eda_robot_move_servos(self, period, up);
    int down[SERVO_COUNT] = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    eda_robot_move_servos(self, period, down);
}

void eda_robot_walk(eda_robot_t *self, float steps, int period, int dir, int amount)
{
    int A[SERVO_COUNT] = {30, 30, 30, 30, 0, 0};
    int O[SERVO_COUNT] = {0, 0, 5, -5, HAND_HOME_POSITION - 90, HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(dir * -90), DEG2RAD(dir * -90), 0, 0};

    if (amount > 0 && self->has_hands) {
        A[LEFT_HAND]  = amount;
        A[RIGHT_HAND] = amount;
        phase_diff[LEFT_HAND]  = phase_diff[RIGHT_LEG];
        phase_diff[RIGHT_HAND] = phase_diff[LEFT_LEG];
    } else {
        A[LEFT_HAND]  = 0;
        A[RIGHT_HAND] = 0;
    }
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_turn(eda_robot_t *self, float steps, int period, int dir, int amount)
{
    int A[SERVO_COUNT] = {30, 30, 30, 30, 0, 0};
    int O[SERVO_COUNT] = {0, 0, 5, -5, HAND_HOME_POSITION - 90, HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(-90), DEG2RAD(-90), 0, 0};

    if (dir == LEFT) {
        A[0] = 30; A[1] = 0;
    } else {
        A[0] = 0;  A[1] = 30;
    }

    if (amount > 0 && self->has_hands) {
        A[LEFT_HAND]  = amount;
        A[RIGHT_HAND] = amount;
        phase_diff[LEFT_HAND]  = phase_diff[LEFT_LEG];
        phase_diff[RIGHT_HAND] = phase_diff[RIGHT_LEG];
    } else {
        A[LEFT_HAND]  = 0;
        A[RIGHT_HAND] = 0;
    }
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_bend(eda_robot_t *self, int steps, int period, int dir)
{
    int bend1[SERVO_COUNT] = {90, 90, 62, 35, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int bend2[SERVO_COUNT] = {90, 90, 62, 105, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int homes[SERVO_COUNT] = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};

    if (dir == -1) {
        bend1[2] = 180 - 35;
        bend1[3] = 180 - 60;
        bend2[2] = 180 - 105;
        bend2[3] = 180 - 60;
    }

    int T2 = 800;
    for (int i = 0; i < steps; i++) {
        eda_robot_move_servos(self, T2 / 2, bend1);
        eda_robot_move_servos(self, T2 / 2, bend2);
        vTaskDelay(pdMS_TO_TICKS((int)(period * 0.8)));
        eda_robot_move_servos(self, 500, homes);
    }
}

void eda_robot_shake_leg(eda_robot_t *self, int steps, int period, int dir)
{
    int numberLegMoves = 2;

    int shake_leg1[SERVO_COUNT] = {90, 90, 58, 35, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int shake_leg2[SERVO_COUNT] = {90, 90, 58, 120, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int shake_leg3[SERVO_COUNT] = {90, 90, 58, 60, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int homes[SERVO_COUNT]      = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};

    if (dir == 1) {
        shake_leg1[2] = 180 - 35;  shake_leg1[3] = 180 - 58;
        shake_leg2[2] = 180 - 120; shake_leg2[3] = 180 - 58;
        shake_leg3[2] = 180 - 60;  shake_leg3[3] = 180 - 58;
    }

    int T2 = 1000;
    period = period - T2;
    period = MAX_VAL(period, 200 * numberLegMoves);

    for (int j = 0; j < steps; j++) {
        eda_robot_move_servos(self, T2 / 2, shake_leg1);
        eda_robot_move_servos(self, T2 / 2, shake_leg2);
        for (int i = 0; i < numberLegMoves; i++) {
            eda_robot_move_servos(self, period / (2 * numberLegMoves), shake_leg3);
            eda_robot_move_servos(self, period / (2 * numberLegMoves), shake_leg2);
        }
        eda_robot_move_servos(self, 500, homes);
    }
    vTaskDelay(pdMS_TO_TICKS(period));
}

void eda_robot_updown(eda_robot_t *self, float steps, int period, int height)
{
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height, -height, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(-90), DEG2RAD(90), 0, 0};
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_swing(eda_robot_t *self, float steps, int period, int height)
{
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height / 2, -height / 2, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(0), DEG2RAD(0), 0, 0};
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_tiptoe_swing(eda_robot_t *self, float steps, int period, int height)
{
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height, -height, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {0, 0, 0, 0, 0, 0};
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_jitter(eda_robot_t *self, float steps, int period, int height)
{
    height = MIN_VAL(25, height);
    int A[SERVO_COUNT] = {height, height, 0, 0, 0, 0};
    int O[SERVO_COUNT] = {0, 0, 0, 0, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(-90), DEG2RAD(90), 0, 0, 0, 0};
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_ascending_turn(eda_robot_t *self, float steps, int period, int height)
{
    height = MIN_VAL(13, height);
    int A[SERVO_COUNT] = {height, height, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height + 4, -height + 4, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(-90), DEG2RAD(90), DEG2RAD(-90), DEG2RAD(90), 0, 0};
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_moonwalker(eda_robot_t *self, float steps, int period, int height, int dir)
{
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height / 2 + 2, -height / 2 - 2, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int phi = -dir * 90;
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(phi), DEG2RAD(-60 * dir + phi), 0, 0};
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_crusaito(eda_robot_t *self, float steps, int period, int height, int dir)
{
    int A[SERVO_COUNT] = {25, 25, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height / 2 + 4, -height / 2 - 4, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {90, 90, DEG2RAD(0), DEG2RAD(-60 * dir), 0, 0};
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_flapping(eda_robot_t *self, float steps, int period, int height, int dir)
{
    int A[SERVO_COUNT] = {12, 12, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height - 10, -height + 10, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(0), DEG2RAD(180), DEG2RAD(-90 * dir), DEG2RAD(90 * dir), 0, 0};
    eda_robot_execute(self, A, O, period, phase_diff, steps);
}

void eda_robot_hands_up(eda_robot_t *self, int period, int dir)
{
    if (!self->has_hands) return;

    int target[SERVO_COUNT] = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};

    if (dir == 0) {
        target[LEFT_HAND]  = 170;
        target[RIGHT_HAND] = 10;
    } else if (dir == 1) {
        target[LEFT_HAND]  = 170;
        target[RIGHT_HAND] = oscillator_get_position(&self->servo[RIGHT_HAND]);
    } else if (dir == -1) {
        target[RIGHT_HAND] = 10;
        target[LEFT_HAND]  = oscillator_get_position(&self->servo[LEFT_HAND]);
    }
    eda_robot_move_servos(self, period, target);
}

void eda_robot_hands_down(eda_robot_t *self, int period, int dir)
{
    if (!self->has_hands) return;

    int target[SERVO_COUNT] = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};

    if (dir == 1)
        target[RIGHT_HAND] = oscillator_get_position(&self->servo[RIGHT_HAND]);
    else if (dir == -1)
        target[LEFT_HAND] = oscillator_get_position(&self->servo[LEFT_HAND]);

    eda_robot_move_servos(self, period, target);
}

static void eda_robot_hand_wave_both(eda_robot_t *self, int period)
{
    if (!self->has_hands) return;

    int cur[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++)
        cur[i] = (self->servo_pins[i] != -1) ? oscillator_get_position(&self->servo[i]) : 90;

    int lp = 170, rp = 10;
    cur[LEFT_HAND] = lp;
    cur[RIGHT_HAND] = rp;
    eda_robot_move_servos(self, 300, cur);

    for (int i = 0; i < 5; i++) {
        cur[LEFT_HAND]  = lp - 30;
        cur[RIGHT_HAND] = rp + 30;
        eda_robot_move_servos(self, period / 10, cur);
        cur[LEFT_HAND]  = lp + 30;
        cur[RIGHT_HAND] = rp - 30;
        eda_robot_move_servos(self, period / 10, cur);
    }

    cur[LEFT_HAND]  = HAND_HOME_POSITION;
    cur[RIGHT_HAND] = 180 - HAND_HOME_POSITION;
    eda_robot_move_servos(self, 300, cur);
}

void eda_robot_hand_wave(eda_robot_t *self, int period, int dir)
{
    if (!self->has_hands) return;

    if (dir == BOTH) {
        eda_robot_hand_wave_both(self, period);
        return;
    }

    int servo_index = (dir == LEFT) ? LEFT_HAND : RIGHT_HAND;

    int cur[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++)
        cur[i] = (self->servo_pins[i] != -1) ? oscillator_get_position(&self->servo[i]) : 90;

    int position = (servo_index == LEFT_HAND) ? 170 : 10;
    cur[servo_index] = position;
    eda_robot_move_servos(self, 300, cur);
    vTaskDelay(pdMS_TO_TICKS(300));

    for (int i = 0; i < 5; i++) {
        if (servo_index == LEFT_HAND) {
            cur[servo_index] = position - 30;
            eda_robot_move_servos(self, period / 10, cur);
            vTaskDelay(pdMS_TO_TICKS(period / 10));
            cur[servo_index] = position + 30;
            eda_robot_move_servos(self, period / 10, cur);
        } else {
            cur[servo_index] = position + 30;
            eda_robot_move_servos(self, period / 10, cur);
            vTaskDelay(pdMS_TO_TICKS(period / 10));
            cur[servo_index] = position - 30;
            eda_robot_move_servos(self, period / 10, cur);
        }
        vTaskDelay(pdMS_TO_TICKS(period / 10));
    }

    cur[servo_index] = (servo_index == LEFT_HAND) ? HAND_HOME_POSITION : (180 - HAND_HOME_POSITION);
    eda_robot_move_servos(self, 300, cur);
}

void eda_robot_enable_servo_limit(eda_robot_t *self, int diff_limit)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_set_limiter(&self->servo[i], diff_limit);
    }
}

void eda_robot_disable_servo_limit(eda_robot_t *self)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1)
            oscillator_disable_limiter(&self->servo[i]);
    }
}
