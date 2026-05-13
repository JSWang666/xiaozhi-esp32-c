/*
 * Otto robot movement functions - Converted from C++ to C.
 */

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define TAG "OttoMovements"

#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef DEG2RAD
#define DEG2RAD(g) ((g) * M_PI / 180.0)
#endif

/* ── Constants ─────────────────────────────────────────── */
#define FORWARD   1
#define BACKWARD -1
#define LEFT      1
#define RIGHT    -1
#define BOTH      0

#define LEFT_LEG    0
#define RIGHT_LEG   1
#define LEFT_FOOT   2
#define RIGHT_FOOT  3
#define LEFT_HAND   4
#define RIGHT_HAND  5
#define SERVO_COUNT 6

#define SERVO_LIMIT_DEFAULT 240
#define HAND_HOME_POSITION 45

/* ── Oscillator type (must match oscillator.c exactly) ── */
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

/* ── Oscillator extern functions (defined in oscillator.c) ── */
extern void oscillator_init(oscillator_t *self, int trim);
extern void oscillator_attach(oscillator_t *self, int pin, bool rev);
extern void oscillator_detach(oscillator_t *self);
extern void oscillator_set_a(oscillator_t *self, unsigned int amplitude);
extern void oscillator_set_o(oscillator_t *self, int offset);
extern void oscillator_set_ph(oscillator_t *self, double ph);
extern void oscillator_set_t(oscillator_t *self, unsigned int period);
extern void oscillator_set_trim(oscillator_t *self, int trim);
extern void oscillator_set_position(oscillator_t *self, int position);
extern void oscillator_refresh(oscillator_t *self);
extern int  oscillator_get_position(oscillator_t *self);
extern void oscillator_set_limiter(oscillator_t *self, int diff_limit);
extern void oscillator_disable_limiter(oscillator_t *self);

/* ── Otto type ─────────────────────────────────────────── */
typedef struct otto_s {
    oscillator_t servo[SERVO_COUNT];
    int servo_pins[SERVO_COUNT];
    int servo_trim[SERVO_COUNT];
    unsigned long final_time;
    unsigned long partial_time;
    float increment[SERVO_COUNT];
    bool is_resting;
    bool has_hands;
} otto_t;

/* ── millis() ──────────────────────────────────────────── */
unsigned long IRAM_ATTR millis(void) {
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

/* ── Otto lifecycle ────────────────────────────────────── */
otto_t *otto_create(void) {
    otto_t *self = (otto_t *)calloc(1, sizeof(otto_t));
    if (!self) return NULL;
    self->is_resting = false;
    self->has_hands = false;
    for (int i = 0; i < SERVO_COUNT; i++) {
        oscillator_init(&self->servo[i], 0);
        self->servo_pins[i] = -1;
        self->servo_trim[i] = 0;
    }
    return self;
}

void otto_destroy(otto_t *self) {
    if (!self) return;
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1) {
            oscillator_detach(&self->servo[i]);
        }
    }
    free(self);
}

void otto_init(otto_t *self, int left_leg, int right_leg,
               int left_foot, int right_foot,
               int left_hand, int right_hand) {
    self->servo_pins[LEFT_LEG]   = left_leg;
    self->servo_pins[RIGHT_LEG]  = right_leg;
    self->servo_pins[LEFT_FOOT]  = left_foot;
    self->servo_pins[RIGHT_FOOT] = right_foot;
    self->servo_pins[LEFT_HAND]  = left_hand;
    self->servo_pins[RIGHT_HAND] = right_hand;

    self->has_hands = (left_hand != -1 && right_hand != -1);

    otto_attach_servos(self);
    self->is_resting = false;
}

void otto_attach_servos(otto_t *self) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1) {
            oscillator_attach(&self->servo[i], self->servo_pins[i], false);
        }
    }
}

void otto_detach_servos(otto_t *self) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1) {
            oscillator_detach(&self->servo[i]);
        }
    }
}

void otto_set_trims(otto_t *self, int left_leg, int right_leg,
                    int left_foot, int right_foot,
                    int left_hand, int right_hand) {
    self->servo_trim[LEFT_LEG]   = left_leg;
    self->servo_trim[RIGHT_LEG]  = right_leg;
    self->servo_trim[LEFT_FOOT]  = left_foot;
    self->servo_trim[RIGHT_FOOT] = right_foot;

    if (self->has_hands) {
        self->servo_trim[LEFT_HAND]  = left_hand;
        self->servo_trim[RIGHT_HAND] = right_hand;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1) {
            oscillator_set_trim(&self->servo[i], self->servo_trim[i]);
        }
    }
}

/* ── Basic motion ──────────────────────────────────────── */
void otto_move_servos(otto_t *self, int time, int servo_target[]) {
    if (self->is_resting) self->is_resting = false;

    self->final_time = millis() + time;
    if (time > 10) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (self->servo_pins[i] != -1) {
                self->increment[i] = (servo_target[i] - oscillator_get_position(&self->servo[i])) / (time / 10.0f);
            }
        }

        for (int iteration = 1; millis() < self->final_time; iteration++) {
            self->partial_time = millis() + 10;
            for (int i = 0; i < SERVO_COUNT; i++) {
                if (self->servo_pins[i] != -1) {
                    oscillator_set_position(&self->servo[i],
                        oscillator_get_position(&self->servo[i]) + self->increment[i]);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (self->servo_pins[i] != -1) {
                oscillator_set_position(&self->servo[i], servo_target[i]);
            }
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
                if (self->servo_pins[i] != -1) {
                    oscillator_set_position(&self->servo[i], servo_target[i]);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            adjustment_count++;
        }
    }
}

void otto_move_single(otto_t *self, int position, int servo_number) {
    if (position > 180) position = 90;
    if (position < 0) position = 90;
    if (self->is_resting) self->is_resting = false;
    if (servo_number >= 0 && servo_number < SERVO_COUNT && self->servo_pins[servo_number] != -1) {
        oscillator_set_position(&self->servo[servo_number], position);
    }
}

void otto_oscillate_servos(otto_t *self, int amplitude[SERVO_COUNT],
                           int offset[SERVO_COUNT], int period,
                           double phase_diff[SERVO_COUNT], float cycle) {
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
            if (self->servo_pins[i] != -1) {
                oscillator_refresh(&self->servo[i]);
            }
        }
        vTaskDelay(5);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

void otto_execute(otto_t *self, int amplitude[SERVO_COUNT],
                  int offset[SERVO_COUNT], int period,
                  double phase_diff[SERVO_COUNT], float steps) {
    if (self->is_resting) self->is_resting = false;

    int cycles = (int)steps;
    if (cycles >= 1) {
        for (int i = 0; i < cycles; i++)
            otto_oscillate_servos(self, amplitude, offset, period, phase_diff, 1.0f);
    }
    otto_oscillate_servos(self, amplitude, offset, period, phase_diff, steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void otto_execute2(otto_t *self, int amplitude[SERVO_COUNT],
                   int center_angle[SERVO_COUNT], int period,
                   double phase_diff[SERVO_COUNT], float steps) {
    if (self->is_resting) self->is_resting = false;

    int offset[SERVO_COUNT];
    for (int i = 0; i < SERVO_COUNT; i++) {
        offset[i] = center_angle[i] - 90;
    }

    int cycles = (int)steps;
    if (cycles >= 1) {
        for (int i = 0; i < cycles; i++)
            otto_oscillate_servos(self, amplitude, offset, period, phase_diff, 1.0f);
    }
    otto_oscillate_servos(self, amplitude, offset, period, phase_diff, steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ── Home ──────────────────────────────────────────────── */
void otto_home(otto_t *self, bool hands_down) {
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
        otto_move_servos(self, 700, homes);
        self->is_resting = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

bool otto_get_rest_state(otto_t *self) { return self->is_resting; }
void otto_set_rest_state(otto_t *self, bool state) { self->is_resting = state; }

/* ── Predetermined motions ─────────────────────────────── */
void otto_jump(otto_t *self, float steps, int period) {
    int up[SERVO_COUNT]   = {90, 90, 150, 30, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    otto_move_servos(self, period, up);
    int down[SERVO_COUNT] = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    otto_move_servos(self, period, down);
}

void otto_walk(otto_t *self, float steps, int period, int dir, int amount) {
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

    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_turn(otto_t *self, float steps, int period, int dir, int amount) {
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

    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_bend(otto_t *self, int steps, int period, int dir) {
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
        otto_move_servos(self, T2 / 2, bend1);
        otto_move_servos(self, T2 / 2, bend2);
        vTaskDelay(pdMS_TO_TICKS((int)(period * 0.8)));
        otto_move_servos(self, 500, homes);
    }
}

void otto_shake_leg(otto_t *self, int steps, int period, int dir) {
    int numberLegMoves = 2;

    int shake_leg1[SERVO_COUNT] = {90, 90, 58, 35, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int shake_leg2[SERVO_COUNT] = {90, 90, 58, 120, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int shake_leg3[SERVO_COUNT] = {90, 90, 58, 60, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int homes[SERVO_COUNT]      = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};

    if (dir == LEFT) {
        shake_leg1[2] = 180 - 35;  shake_leg1[3] = 180 - 58;
        shake_leg2[2] = 180 - 120; shake_leg2[3] = 180 - 58;
        shake_leg3[2] = 180 - 60;  shake_leg3[3] = 180 - 58;
    }

    int T2 = 1000;
    period = period - T2;
    period = MAX(period, 200 * numberLegMoves);

    for (int j = 0; j < steps; j++) {
        otto_move_servos(self, T2 / 2, shake_leg1);
        otto_move_servos(self, T2 / 2, shake_leg2);
        for (int i = 0; i < numberLegMoves; i++) {
            otto_move_servos(self, period / (2 * numberLegMoves), shake_leg3);
            otto_move_servos(self, period / (2 * numberLegMoves), shake_leg2);
        }
        otto_move_servos(self, 500, homes);
    }
    vTaskDelay(pdMS_TO_TICKS(period));
}

void otto_sit(otto_t *self) {
    int target[SERVO_COUNT] = {120, 60, 0, 180, 45, 135};
    otto_move_servos(self, 600, target);
}

void otto_updown(otto_t *self, float steps, int period, int height) {
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height, -height, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(-90), DEG2RAD(90), 0, 0};
    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_swing(otto_t *self, float steps, int period, int height) {
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height / 2, -height / 2, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(0), DEG2RAD(0), 0, 0};
    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_tiptoe_swing(otto_t *self, float steps, int period, int height) {
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height, -height, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {0, 0, 0, 0, 0, 0};
    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_jitter(otto_t *self, float steps, int period, int height) {
    height = MIN(25, height);
    int A[SERVO_COUNT] = {height, height, 0, 0, 0, 0};
    int O[SERVO_COUNT] = {0, 0, 0, 0, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(-90), DEG2RAD(90), 0, 0, 0, 0};
    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_ascending_turn(otto_t *self, float steps, int period, int height) {
    height = MIN(13, height);
    int A[SERVO_COUNT] = {height, height, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height + 4, -height + 4, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(-90), DEG2RAD(90), DEG2RAD(-90), DEG2RAD(90), 0, 0};
    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_moonwalker(otto_t *self, float steps, int period, int height, int dir) {
    int A[SERVO_COUNT] = {0, 0, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height / 2 + 2, -height / 2 - 2, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    int phi = -dir * 90;
    double phase_diff[SERVO_COUNT] = {0, 0, DEG2RAD(phi), DEG2RAD(-60 * dir + phi), 0, 0};
    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_crusaito(otto_t *self, float steps, int period, int height, int dir) {
    int A[SERVO_COUNT] = {25, 25, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height / 2 + 4, -height / 2 - 4, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {90, 90, DEG2RAD(0), DEG2RAD(-60 * dir), 0, 0};
    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_flapping(otto_t *self, float steps, int period, int height, int dir) {
    int A[SERVO_COUNT] = {12, 12, height, height, 0, 0};
    int O[SERVO_COUNT] = {0, 0, height - 10, -height + 10, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(0), DEG2RAD(180), DEG2RAD(-90 * dir), DEG2RAD(90 * dir), 0, 0};
    otto_execute(self, A, O, period, phase_diff, steps);
}

void otto_whirlwind_leg(otto_t *self, float steps, int period, int amplitude) {
    int target[SERVO_COUNT] = {90, 90, 180, 90, 45, 20};
    otto_move_servos(self, 100, target);
    target[RIGHT_FOOT] = 160;
    otto_move_servos(self, 500, target);
    vTaskDelay(pdMS_TO_TICKS(1000));

    int C[SERVO_COUNT] = {90, 90, 180, 160, 45, 20};
    int A[SERVO_COUNT] = {amplitude, 0, 0, 0, amplitude, 0};
    double phase_diff[SERVO_COUNT] = {DEG2RAD(20), 0, 0, 0, DEG2RAD(20), 0};
    otto_execute2(self, A, C, period, phase_diff, steps);
}

/* ── Hand motions ──────────────────────────────────────── */
void otto_hands_up(otto_t *self, int period, int dir) {
    if (!self->has_hands) return;

    int target[SERVO_COUNT] = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    if (dir == 0) {
        target[LEFT_HAND]  = 170;
        target[RIGHT_HAND] = 10;
    } else if (dir == LEFT) {
        target[LEFT_HAND]  = 170;
        target[RIGHT_HAND] = oscillator_get_position(&self->servo[RIGHT_HAND]);
    } else if (dir == RIGHT) {
        target[RIGHT_HAND] = 10;
        target[LEFT_HAND]  = oscillator_get_position(&self->servo[LEFT_HAND]);
    }
    otto_move_servos(self, period, target);
}

void otto_hands_down(otto_t *self, int period, int dir) {
    if (!self->has_hands) return;

    int target[SERVO_COUNT] = {90, 90, 90, 90, HAND_HOME_POSITION, 180 - HAND_HOME_POSITION};
    if (dir == LEFT) {
        target[RIGHT_HAND] = oscillator_get_position(&self->servo[RIGHT_HAND]);
    } else if (dir == RIGHT) {
        target[LEFT_HAND] = oscillator_get_position(&self->servo[LEFT_HAND]);
    }
    otto_move_servos(self, period, target);
}

void otto_hand_wave(otto_t *self, int dir) {
    if (!self->has_hands) return;

    if (dir == LEFT) {
        int C[SERVO_COUNT] = {90, 90, 90, 90, 160, 135};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 20, 0};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, DEG2RAD(90), 0};
        otto_execute2(self, A, C, 300, ph, 5);
    } else if (dir == RIGHT) {
        int C[SERVO_COUNT] = {90, 90, 90, 90, 45, 20};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 0, 20};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, 0, DEG2RAD(90)};
        otto_execute2(self, A, C, 300, ph, 5);
    } else {
        int C[SERVO_COUNT] = {90, 90, 90, 90, 160, 20};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 20, 20};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, DEG2RAD(90), DEG2RAD(90)};
        otto_execute2(self, A, C, 300, ph, 5);
    }
}

void otto_windmill(otto_t *self, float steps, int period, int amplitude) {
    if (!self->has_hands) return;
    int C[SERVO_COUNT] = {90, 90, 90, 90, 90, 90};
    int A[SERVO_COUNT] = {0, 0, 0, 0, amplitude, amplitude};
    double ph[SERVO_COUNT] = {0, 0, 0, 0, DEG2RAD(90), DEG2RAD(90)};
    otto_execute2(self, A, C, period, ph, steps);
}

void otto_takeoff(otto_t *self, float steps, int period, int amplitude) {
    if (!self->has_hands) return;
    otto_home(self, true);
    int C[SERVO_COUNT] = {90, 90, 90, 90, 90, 90};
    int A[SERVO_COUNT] = {0, 0, 0, 0, amplitude, amplitude};
    double ph[SERVO_COUNT] = {0, 0, 0, 0, DEG2RAD(90), DEG2RAD(-90)};
    otto_execute2(self, A, C, period, ph, steps);
}

void otto_fitness(otto_t *self, float steps, int period, int amplitude) {
    if (!self->has_hands) return;
    int target[SERVO_COUNT] = {90, 90, 90, 0, 160, 135};
    otto_move_servos(self, 100, target);
    target[LEFT_FOOT] = 20;
    otto_move_servos(self, 400, target);
    vTaskDelay(pdMS_TO_TICKS(2000));

    int C[SERVO_COUNT] = {90, 90, 20, 90, 160, 135};
    int A[SERVO_COUNT] = {0, 0, 0, 0, 0, amplitude};
    double ph[SERVO_COUNT] = {0, 0, 0, 0, 0, 0};
    otto_execute2(self, A, C, period, ph, steps);
}

void otto_greeting(otto_t *self, int dir, float steps) {
    if (!self->has_hands) return;
    if (dir == LEFT) {
        int target[SERVO_COUNT] = {90, 90, 150, 150, 45, 135};
        otto_move_servos(self, 400, target);
        int C[SERVO_COUNT] = {90, 90, 150, 150, 160, 135};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 20, 0};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, 0, 0};
        otto_execute2(self, A, C, 300, ph, steps);
    } else if (dir == RIGHT) {
        int target[SERVO_COUNT] = {90, 90, 30, 30, 45, 135};
        otto_move_servos(self, 400, target);
        int C[SERVO_COUNT] = {90, 90, 30, 30, 45, 20};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 0, 20};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, 0, 0};
        otto_execute2(self, A, C, 300, ph, steps);
    }
}

void otto_shy(otto_t *self, int dir, float steps) {
    if (!self->has_hands) return;
    if (dir == LEFT) {
        int target[SERVO_COUNT] = {90, 90, 150, 150, 45, 135};
        otto_move_servos(self, 400, target);
        int C[SERVO_COUNT] = {90, 90, 150, 150, 45, 135};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 20, 20};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, DEG2RAD(90), DEG2RAD(-90)};
        otto_execute2(self, A, C, 300, ph, steps);
    } else if (dir == RIGHT) {
        int target[SERVO_COUNT] = {90, 90, 30, 30, 45, 135};
        otto_move_servos(self, 400, target);
        int C[SERVO_COUNT] = {90, 90, 30, 30, 45, 135};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 0, 20};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, DEG2RAD(90), DEG2RAD(-90)};
        otto_execute2(self, A, C, 300, ph, steps);
    }
}

void otto_radio_calisthenics(otto_t *self) {
    if (!self->has_hands) return;

    const int period = 1000;
    const float steps = 8.0f;

    {
        int C[SERVO_COUNT] = {90, 90, 90, 90, 145, 45};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 45, 45};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, DEG2RAD(90), DEG2RAD(-90)};
        otto_execute2(self, A, C, period, ph, steps);
    }
    {
        int C[SERVO_COUNT] = {90, 90, 115, 65, 90, 90};
        int A[SERVO_COUNT] = {0, 0, 25, 25, 0, 0};
        double ph[SERVO_COUNT] = {0, 0, DEG2RAD(90), DEG2RAD(-90), 0, 0};
        otto_execute2(self, A, C, period, ph, steps);
    }
    {
        int C[SERVO_COUNT] = {90, 90, 130, 130, 90, 90};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 20, 0};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, 0, 0};
        otto_execute2(self, A, C, period, ph, steps);
    }
    {
        int C[SERVO_COUNT] = {90, 90, 50, 50, 90, 90};
        int A[SERVO_COUNT] = {0, 0, 0, 0, 0, 20};
        double ph[SERVO_COUNT] = {0, 0, 0, 0, 0, 0};
        otto_execute2(self, A, C, period, ph, steps);
    }
}

void otto_magic_circle(otto_t *self) {
    if (!self->has_hands) return;
    int A[SERVO_COUNT] = {30, 30, 30, 30, 50, 50};
    int O[SERVO_COUNT] = {0, 0, 5, -5, 0, 0};
    double ph[SERVO_COUNT] = {0, 0, DEG2RAD(-90), DEG2RAD(-90), DEG2RAD(-90), DEG2RAD(90)};
    otto_execute(self, A, O, 700, ph, 40);
}

void otto_showcase(otto_t *self) {
    if (self->is_resting) self->is_resting = false;

    otto_walk(self, 3, 1000, FORWARD, 50);
    vTaskDelay(pdMS_TO_TICKS(500));

    if (self->has_hands) {
        otto_hand_wave(self, LEFT);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (self->has_hands) {
        otto_radio_calisthenics(self);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    otto_moonwalker(self, 3, 900, 25, LEFT);
    vTaskDelay(pdMS_TO_TICKS(500));

    otto_swing(self, 3, 1000, 30);
    vTaskDelay(pdMS_TO_TICKS(500));

    if (self->has_hands) {
        otto_takeoff(self, 5, 300, 40);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (self->has_hands) {
        otto_fitness(self, 5, 1000, 25);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    otto_walk(self, 3, 1000, BACKWARD, 50);
}

/* ── Servo limiter ─────────────────────────────────────── */
void otto_enable_servo_limit(otto_t *self, int diff_limit) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1) {
            oscillator_set_limiter(&self->servo[i], diff_limit);
        }
    }
}

void otto_disable_servo_limit(otto_t *self) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (self->servo_pins[i] != -1) {
            oscillator_disable_limiter(&self->servo[i]);
        }
    }
}
