/*
 * Otto robot controller - MCP protocol version.
 * Converted from C++ to C.
 */

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "settings.h"
#include "c_api/mcp_server_c_api.h"

#define TAG "OttoController"

#define SERVO_COUNT 6
#define LEFT_LEG    0
#define RIGHT_LEG   1
#define LEFT_FOOT   2
#define RIGHT_FOOT  3
#define LEFT_HAND   4
#define RIGHT_HAND  5

#define FORWARD   1
#define BACKWARD -1
#define LEFT      1
#define RIGHT    -1

/* ── Opaque otto_t (defined in otto_movements.c) ───────── */
typedef struct otto_s otto_t;

extern otto_t *otto_create(void);
extern void    otto_destroy(otto_t *self);
extern void    otto_init(otto_t *self, int ll, int rl, int lf, int rf, int lh, int rh);
extern void    otto_attach_servos(otto_t *self);
extern void    otto_detach_servos(otto_t *self);
extern void    otto_set_trims(otto_t *self, int ll, int rl, int lf, int rf, int lh, int rh);
extern void    otto_move_servos(otto_t *self, int time, int servo_target[]);
extern void    otto_execute2(otto_t *self, int amplitude[], int center_angle[], int period, double phase_diff[], float steps);
extern void    otto_home(otto_t *self, bool hands_down);
extern void    otto_jump(otto_t *self, float steps, int period);
extern void    otto_walk(otto_t *self, float steps, int period, int dir, int amount);
extern void    otto_turn(otto_t *self, float steps, int period, int dir, int amount);
extern void    otto_bend(otto_t *self, int steps, int period, int dir);
extern void    otto_shake_leg(otto_t *self, int steps, int period, int dir);
extern void    otto_sit(otto_t *self);
extern void    otto_updown(otto_t *self, float steps, int period, int height);
extern void    otto_swing(otto_t *self, float steps, int period, int height);
extern void    otto_tiptoe_swing(otto_t *self, float steps, int period, int height);
extern void    otto_jitter(otto_t *self, float steps, int period, int height);
extern void    otto_ascending_turn(otto_t *self, float steps, int period, int height);
extern void    otto_moonwalker(otto_t *self, float steps, int period, int height, int dir);
extern void    otto_crusaito(otto_t *self, float steps, int period, int height, int dir);
extern void    otto_flapping(otto_t *self, float steps, int period, int height, int dir);
extern void    otto_whirlwind_leg(otto_t *self, float steps, int period, int amplitude);
extern void    otto_hands_up(otto_t *self, int period, int dir);
extern void    otto_hands_down(otto_t *self, int period, int dir);
extern void    otto_hand_wave(otto_t *self, int dir);
extern void    otto_windmill(otto_t *self, float steps, int period, int amplitude);
extern void    otto_takeoff(otto_t *self, float steps, int period, int amplitude);
extern void    otto_fitness(otto_t *self, float steps, int period, int amplitude);
extern void    otto_greeting(otto_t *self, int dir, float steps);
extern void    otto_shy(otto_t *self, int dir, float steps);
extern void    otto_radio_calisthenics(otto_t *self);
extern void    otto_magic_circle(otto_t *self);
extern void    otto_showcase(otto_t *self);

/* ── Action types ──────────────────────────────────────── */
enum {
    ACTION_WALK = 1,
    ACTION_TURN = 2,
    ACTION_JUMP = 3,
    ACTION_SWING = 4,
    ACTION_MOONWALK = 5,
    ACTION_BEND = 6,
    ACTION_SHAKE_LEG = 7,
    ACTION_UPDOWN = 8,
    ACTION_TIPTOE_SWING = 9,
    ACTION_JITTER = 10,
    ACTION_ASCENDING_TURN = 11,
    ACTION_CRUSAITO = 12,
    ACTION_FLAPPING = 13,
    ACTION_HANDS_UP = 14,
    ACTION_HANDS_DOWN = 15,
    ACTION_HAND_WAVE = 16,
    ACTION_HOME = 17,
    ACTION_SERVO_SEQUENCE = 18,
    ACTION_WHIRLWIND_LEG = 19,
    ACTION_WINDMILL = 20,
    ACTION_TAKEOFF = 21,
    ACTION_FITNESS = 22,
    ACTION_GREETING = 23,
    ACTION_SHY = 24,
    ACTION_SIT = 25,
    ACTION_RADIO_CALISTHENICS = 26,
    ACTION_MAGIC_CIRCLE = 27,
    ACTION_SHOWCASE = 28,
};

typedef struct {
    int action_type;
    int steps;
    int speed;
    int direction;
    int amount;
    char servo_sequence_json[512];
} otto_action_params_t;

typedef struct {
    otto_t *otto;
    TaskHandle_t action_task;
    QueueHandle_t action_queue;
    bool has_hands;
    bool is_action_in_progress;
} otto_controller_t;

static otto_controller_t *g_ctrl = NULL;

/* ── Forward declarations ──────────────────────────────── */
static void action_task_fn(void *arg);
static void start_action_task(void);
static void queue_action(int action_type, int steps, int speed, int direction, int amount);
static void queue_servo_sequence(const char *json_str);
static void load_trims_from_nvs(void);
static void register_mcp_tools(void);

/* ── Helpers ───────────────────────────────────────────── */

#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool is_hand_action(int t) {
    return (t >= ACTION_HANDS_UP && t <= ACTION_HAND_WAVE) ||
           t == ACTION_WINDMILL || t == ACTION_TAKEOFF ||
           t == ACTION_FITNESS  || t == ACTION_GREETING ||
           t == ACTION_SHY || t == ACTION_RADIO_CALISTHENICS ||
           t == ACTION_MAGIC_CIRCLE;
}

/* ── Servo sequence execution (extracted from ActionTask) ── */
static void execute_servo_sequence(otto_controller_t *ctrl, const char *json_buf) {
    cJSON *json = cJSON_Parse(json_buf);
    if (!json) {
        const char *err = cJSON_GetErrorPtr();
        int len = (int)strlen(json_buf);
        ESP_LOGE(TAG, "解析舵机序列JSON失败，长度=%d，错误位置: %s", len, err ? err : "未知");
        ESP_LOGE(TAG, "JSON内容: %s", json_buf);
        return;
    }

    cJSON *actions = cJSON_GetObjectItem(json, "a");
    if (!cJSON_IsArray(actions)) {
        ESP_LOGE(TAG, "舵机序列格式错误: 'a'不是数组");
        cJSON_Delete(json);
        return;
    }

    int array_size = cJSON_GetArraySize(actions);
    ESP_LOGI(TAG, "执行舵机序列，共%d个动作", array_size);

    int sequence_delay = 0;
    cJSON *sd = cJSON_GetObjectItem(json, "d");
    if (cJSON_IsNumber(sd)) {
        sequence_delay = sd->valueint;
        if (sequence_delay < 0) sequence_delay = 0;
    }

    int current_positions[SERVO_COUNT];
    for (int j = 0; j < SERVO_COUNT; j++) current_positions[j] = 90;
    current_positions[LEFT_HAND]  = 45;
    current_positions[RIGHT_HAND] = 180 - 45;

    const char *servo_names[] = {"ll", "rl", "lf", "rf", "lh", "rh"};

    for (int i = 0; i < array_size; i++) {
        cJSON *action_item = cJSON_GetArrayItem(actions, i);
        if (!cJSON_IsObject(action_item)) continue;

        cJSON *osc_item = cJSON_GetObjectItem(action_item, "osc");
        if (cJSON_IsObject(osc_item)) {
            int amplitude[SERVO_COUNT] = {0};
            int center_angle[SERVO_COUNT] = {0};
            double phase_diff[SERVO_COUNT] = {0};
            int period = 300;
            float steps = 8.0f;

            for (int j = 0; j < SERVO_COUNT; j++) {
                amplitude[j] = 0;
                center_angle[j] = 90;
            }

            cJSON *amp_obj = cJSON_GetObjectItem(osc_item, "a");
            if (cJSON_IsObject(amp_obj)) {
                for (int j = 0; j < SERVO_COUNT; j++) {
                    cJSON *v = cJSON_GetObjectItem(amp_obj, servo_names[j]);
                    if (cJSON_IsNumber(v) && v->valueint >= 10 && v->valueint <= 90)
                        amplitude[j] = v->valueint;
                }
            }

            cJSON *center_obj = cJSON_GetObjectItem(osc_item, "o");
            if (cJSON_IsObject(center_obj)) {
                for (int j = 0; j < SERVO_COUNT; j++) {
                    cJSON *v = cJSON_GetObjectItem(center_obj, servo_names[j]);
                    if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 180)
                        center_angle[j] = v->valueint;
                }
            }

            const int LARGE_AMPLITUDE_THRESHOLD = 40;
            if (amplitude[LEFT_LEG]  >= LARGE_AMPLITUDE_THRESHOLD &&
                amplitude[RIGHT_LEG] >= LARGE_AMPLITUDE_THRESHOLD) {
                ESP_LOGW(TAG, "检测到左右腿同时大幅度振荡，限制右腿振幅");
                amplitude[RIGHT_LEG] = 0;
            }
            if (amplitude[LEFT_FOOT]  >= LARGE_AMPLITUDE_THRESHOLD &&
                amplitude[RIGHT_FOOT] >= LARGE_AMPLITUDE_THRESHOLD) {
                ESP_LOGW(TAG, "检测到左右脚同时大幅度振荡，限制右脚振幅");
                amplitude[RIGHT_FOOT] = 0;
            }

            cJSON *phase_obj = cJSON_GetObjectItem(osc_item, "ph");
            if (cJSON_IsObject(phase_obj)) {
                for (int j = 0; j < SERVO_COUNT; j++) {
                    cJSON *v = cJSON_GetObjectItem(phase_obj, servo_names[j]);
                    if (cJSON_IsNumber(v))
                        phase_diff[j] = v->valuedouble * M_PI / 180.0;
                }
            }

            cJSON *p = cJSON_GetObjectItem(osc_item, "p");
            if (cJSON_IsNumber(p)) {
                period = p->valueint;
                if (period < 100) period = 100;
                if (period > 3000) period = 3000;
            }

            cJSON *c = cJSON_GetObjectItem(osc_item, "c");
            if (cJSON_IsNumber(c)) {
                steps = (float)c->valuedouble;
                if (steps < 0.1f) steps = 0.1f;
                if (steps > 20.0f) steps = 20.0f;
            }

            ESP_LOGI(TAG, "执行振荡动作%d: period=%d, steps=%.1f", i, period, steps);
            otto_execute2(ctrl->otto, amplitude, center_angle, period, phase_diff, steps);

            for (int j = 0; j < SERVO_COUNT; j++)
                current_positions[j] = center_angle[j];
        } else {
            int servo_target[SERVO_COUNT];
            for (int j = 0; j < SERVO_COUNT; j++)
                servo_target[j] = current_positions[j];

            cJSON *servos = cJSON_GetObjectItem(action_item, "s");
            if (cJSON_IsObject(servos)) {
                for (int j = 0; j < SERVO_COUNT; j++) {
                    cJSON *v = cJSON_GetObjectItem(servos, servo_names[j]);
                    if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 180)
                        servo_target[j] = v->valueint;
                }
            }

            int speed = 1000;
            cJSON *sp = cJSON_GetObjectItem(action_item, "v");
            if (cJSON_IsNumber(sp)) {
                speed = sp->valueint;
                if (speed < 100) speed = 100;
                if (speed > 3000) speed = 3000;
            }

            ESP_LOGI(TAG, "执行动作%d: ll=%d, rl=%d, lf=%d, rf=%d, v=%d",
                     i, servo_target[LEFT_LEG], servo_target[RIGHT_LEG],
                     servo_target[LEFT_FOOT], servo_target[RIGHT_FOOT], speed);
            otto_move_servos(ctrl->otto, speed, servo_target);

            for (int j = 0; j < SERVO_COUNT; j++)
                current_positions[j] = servo_target[j];
        }

        int delay_after = 0;
        cJSON *d = cJSON_GetObjectItem(action_item, "d");
        if (cJSON_IsNumber(d)) {
            delay_after = d->valueint;
            if (delay_after < 0) delay_after = 0;
        }
        if (delay_after > 0 && i < array_size - 1) {
            ESP_LOGI(TAG, "动作%d执行完成，延迟%d毫秒", i, delay_after);
            vTaskDelay(pdMS_TO_TICKS(delay_after));
        }
    }

    if (sequence_delay > 0) {
        UBaseType_t qc = uxQueueMessagesWaiting(ctrl->action_queue);
        if (qc > 0) {
            ESP_LOGI(TAG, "序列执行完成，延迟%d毫秒后执行下一个序列（队列中还有%d个序列）",
                     sequence_delay, (int)qc);
            vTaskDelay(pdMS_TO_TICKS(sequence_delay));
        }
    }
    cJSON_Delete(json);
}

/* ── Action task ───────────────────────────────────────── */
static void action_task_fn(void *arg) {
    otto_controller_t *ctrl = (otto_controller_t *)arg;
    otto_action_params_t params;
    otto_attach_servos(ctrl->otto);

    while (true) {
        if (xQueueReceive(ctrl->action_queue, &params, pdMS_TO_TICKS(1000)) != pdTRUE)
            continue;

        ESP_LOGI(TAG, "执行动作: %d", params.action_type);
        ctrl->is_action_in_progress = true;

        if (params.action_type == ACTION_SERVO_SEQUENCE) {
            execute_servo_sequence(ctrl, params.servo_sequence_json);
        } else {
            switch (params.action_type) {
                case ACTION_WALK:
                    otto_walk(ctrl->otto, params.steps, params.speed, params.direction, params.amount);
                    break;
                case ACTION_TURN:
                    otto_turn(ctrl->otto, params.steps, params.speed, params.direction, params.amount);
                    break;
                case ACTION_JUMP:
                    otto_jump(ctrl->otto, params.steps, params.speed);
                    break;
                case ACTION_SWING:
                    otto_swing(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_MOONWALK:
                    otto_moonwalker(ctrl->otto, params.steps, params.speed, params.amount, params.direction);
                    break;
                case ACTION_BEND:
                    otto_bend(ctrl->otto, params.steps, params.speed, params.direction);
                    break;
                case ACTION_SHAKE_LEG:
                    otto_shake_leg(ctrl->otto, params.steps, params.speed, params.direction);
                    break;
                case ACTION_SIT:
                    otto_sit(ctrl->otto);
                    break;
                case ACTION_RADIO_CALISTHENICS:
                    if (ctrl->has_hands) otto_radio_calisthenics(ctrl->otto);
                    break;
                case ACTION_MAGIC_CIRCLE:
                    if (ctrl->has_hands) otto_magic_circle(ctrl->otto);
                    break;
                case ACTION_SHOWCASE:
                    otto_showcase(ctrl->otto);
                    break;
                case ACTION_UPDOWN:
                    otto_updown(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_TIPTOE_SWING:
                    otto_tiptoe_swing(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_JITTER:
                    otto_jitter(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_ASCENDING_TURN:
                    otto_ascending_turn(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_CRUSAITO:
                    otto_crusaito(ctrl->otto, params.steps, params.speed, params.amount, params.direction);
                    break;
                case ACTION_FLAPPING:
                    otto_flapping(ctrl->otto, params.steps, params.speed, params.amount, params.direction);
                    break;
                case ACTION_WHIRLWIND_LEG:
                    otto_whirlwind_leg(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_HANDS_UP:
                    if (ctrl->has_hands) otto_hands_up(ctrl->otto, params.speed, params.direction);
                    break;
                case ACTION_HANDS_DOWN:
                    if (ctrl->has_hands) otto_hands_down(ctrl->otto, params.speed, params.direction);
                    break;
                case ACTION_HAND_WAVE:
                    if (ctrl->has_hands) otto_hand_wave(ctrl->otto, params.direction);
                    break;
                case ACTION_WINDMILL:
                    if (ctrl->has_hands) otto_windmill(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_TAKEOFF:
                    if (ctrl->has_hands) otto_takeoff(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_FITNESS:
                    if (ctrl->has_hands) otto_fitness(ctrl->otto, params.steps, params.speed, params.amount);
                    break;
                case ACTION_GREETING:
                    if (ctrl->has_hands) otto_greeting(ctrl->otto, params.direction, params.steps);
                    break;
                case ACTION_SHY:
                    if (ctrl->has_hands) otto_shy(ctrl->otto, params.direction, params.steps);
                    break;
                case ACTION_HOME:
                    otto_home(ctrl->otto, true);
                    break;
            }
            if (params.action_type != ACTION_SIT) {
                if (params.action_type != ACTION_HOME && params.action_type != ACTION_SERVO_SEQUENCE) {
                    otto_home(ctrl->otto, params.action_type != ACTION_HANDS_UP);
                }
            }
        }
        ctrl->is_action_in_progress = false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void start_action_task(void) {
    if (!g_ctrl || g_ctrl->action_task) return;
    xTaskCreate(action_task_fn, "otto_action", 1024 * 3, g_ctrl,
                configMAX_PRIORITIES - 1, &g_ctrl->action_task);
}

static void queue_action(int action_type, int steps, int speed, int direction, int amount) {
    if (!g_ctrl) return;

    if (is_hand_action(action_type) && !g_ctrl->has_hands) {
        ESP_LOGW(TAG, "尝试执行手部动作，但机器人没有配置手部舵机");
        return;
    }

    ESP_LOGI(TAG, "动作控制: 类型=%d, 步数=%d, 速度=%d, 方向=%d, 幅度=%d",
             action_type, steps, speed, direction, amount);

    otto_action_params_t p;
    memset(&p, 0, sizeof(p));
    p.action_type = action_type;
    p.steps     = steps;
    p.speed     = speed;
    p.direction = direction;
    p.amount    = amount;

    xQueueSend(g_ctrl->action_queue, &p, portMAX_DELAY);
    start_action_task();
}

static void queue_servo_sequence(const char *json_str) {
    if (!g_ctrl || !json_str) {
        ESP_LOGE(TAG, "序列JSON为空");
        return;
    }

    int input_len = (int)strlen(json_str);
    const int buffer_size = 512;
    ESP_LOGI(TAG, "队列舵机序列，输入长度=%d，缓冲区大小=%d", input_len, buffer_size);

    if (input_len >= buffer_size) {
        ESP_LOGE(TAG, "JSON字符串太长！输入长度=%d，最大允许=%d", input_len, buffer_size - 1);
        return;
    }
    if (input_len == 0) {
        ESP_LOGW(TAG, "序列JSON为空字符串");
        return;
    }

    otto_action_params_t p;
    memset(&p, 0, sizeof(p));
    p.action_type = ACTION_SERVO_SEQUENCE;
    strncpy(p.servo_sequence_json, json_str, sizeof(p.servo_sequence_json) - 1);
    p.servo_sequence_json[sizeof(p.servo_sequence_json) - 1] = '\0';

    ESP_LOGD(TAG, "序列已加入队列: %s", p.servo_sequence_json);
    xQueueSend(g_ctrl->action_queue, &p, portMAX_DELAY);
    start_action_task();
}

static void load_trims_from_nvs(void) {
    settings_t *s = settings_open("otto_trims", false);
    int ll = settings_get_int(s, "left_leg", 0);
    int rl = settings_get_int(s, "right_leg", 0);
    int lf = settings_get_int(s, "left_foot", 0);
    int rf = settings_get_int(s, "right_foot", 0);
    int lh = settings_get_int(s, "left_hand", 0);
    int rh = settings_get_int(s, "right_hand", 0);
    settings_close(s);

    ESP_LOGI(TAG, "从NVS加载微调设置: 左腿=%d, 右腿=%d, 左脚=%d, 右脚=%d, 左手=%d, 右手=%d",
             ll, rl, lf, rf, lh, rh);
    otto_set_trims(g_ctrl->otto, ll, rl, lf, rf, lh, rh);
}

/* ── MCP tool callbacks ────────────────────────────────── */

static mcp_tool_result_t tool_action_cb(const void *args, void *ud) {
    (void)ud;
    cJSON *root = (cJSON *)args;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };

    cJSON *a = cJSON_GetObjectItem(root, "action");
    const char *action = (a && cJSON_IsString(a)) ? a->valuestring : "sit";

    cJSON *st = cJSON_GetObjectItem(root, "steps");
    int steps = (st && cJSON_IsNumber(st)) ? st->valueint : 3;

    cJSON *sp = cJSON_GetObjectItem(root, "speed");
    int speed = (sp && cJSON_IsNumber(sp)) ? sp->valueint : 700;

    cJSON *di = cJSON_GetObjectItem(root, "direction");
    int direction = (di && cJSON_IsNumber(di)) ? di->valueint : 1;

    cJSON *am = cJSON_GetObjectItem(root, "amount");
    int amount = (am && cJSON_IsNumber(am)) ? am->valueint : 30;

    cJSON *as = cJSON_GetObjectItem(root, "arm_swing");
    int arm_swing = (as && cJSON_IsNumber(as)) ? as->valueint : 50;

    if (strcmp(action, "walk") == 0) { queue_action(ACTION_WALK, steps, speed, direction, arm_swing); }
    else if (strcmp(action, "turn") == 0) { queue_action(ACTION_TURN, steps, speed, direction, arm_swing); }
    else if (strcmp(action, "jump") == 0) { queue_action(ACTION_JUMP, steps, speed, 0, 0); }
    else if (strcmp(action, "swing") == 0) { queue_action(ACTION_SWING, steps, speed, 0, amount); }
    else if (strcmp(action, "moonwalk") == 0) { queue_action(ACTION_MOONWALK, steps, speed, direction, amount); }
    else if (strcmp(action, "bend") == 0) { queue_action(ACTION_BEND, steps, speed, direction, 0); }
    else if (strcmp(action, "shake_leg") == 0) { queue_action(ACTION_SHAKE_LEG, steps, speed, direction, 0); }
    else if (strcmp(action, "updown") == 0) { queue_action(ACTION_UPDOWN, steps, speed, 0, amount); }
    else if (strcmp(action, "whirlwind_leg") == 0) { queue_action(ACTION_WHIRLWIND_LEG, steps, speed, 0, amount); }
    else if (strcmp(action, "sit") == 0) { queue_action(ACTION_SIT, 1, 0, 0, 0); }
    else if (strcmp(action, "showcase") == 0) { queue_action(ACTION_SHOWCASE, 1, 0, 0, 0); }
    else if (strcmp(action, "home") == 0) { queue_action(ACTION_HOME, 1, 1000, 1, 0); }
    else if (strcmp(action, "hands_up") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_HANDS_UP, 1, speed, direction, 0);
    }
    else if (strcmp(action, "hands_down") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_HANDS_DOWN, 1, speed, direction, 0);
    }
    else if (strcmp(action, "hand_wave") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_HAND_WAVE, 1, 0, 0, direction);
    }
    else if (strcmp(action, "windmill") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_WINDMILL, steps, speed, 0, amount);
    }
    else if (strcmp(action, "takeoff") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_TAKEOFF, steps, speed, 0, amount);
    }
    else if (strcmp(action, "fitness") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_FITNESS, steps, speed, 0, amount);
    }
    else if (strcmp(action, "greeting") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_GREETING, steps, 0, direction, 0);
    }
    else if (strcmp(action, "shy") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_SHY, steps, 0, direction, 0);
    }
    else if (strcmp(action, "radio_calisthenics") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_RADIO_CALISTHENICS, 1, 0, 0, 0);
    }
    else if (strcmp(action, "magic_circle") == 0) {
        if (!g_ctrl->has_hands) { res.is_error = true; res.text = strdup("错误：此动作需要手部舵机支持"); return res; }
        queue_action(ACTION_MAGIC_CIRCLE, 1, 0, 0, 0);
    }
    else {
        res.is_error = true;
        res.text = strdup("错误：无效的动作名称。可用动作：walk, turn, jump, swing, moonwalk, bend, shake_leg, updown, whirlwind_leg, sit, showcase, home, hands_up, hands_down, hand_wave, windmill, takeoff, fitness, greeting, shy, radio_calisthenics, magic_circle");
    }
    return res;
}

static mcp_tool_result_t tool_servo_sequences_cb(const void *args, void *ud) {
    (void)ud;
    cJSON *root = (cJSON *)args;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };

    cJSON *seq = cJSON_GetObjectItem(root, "sequence");
    if (seq && cJSON_IsString(seq)) {
        queue_servo_sequence(seq->valuestring);
    }
    return res;
}

static mcp_tool_result_t tool_stop_cb(const void *args, void *ud) {
    (void)args; (void)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };

    if (g_ctrl->action_task) {
        vTaskDelete(g_ctrl->action_task);
        g_ctrl->action_task = NULL;
    }
    g_ctrl->is_action_in_progress = false;
    xQueueReset(g_ctrl->action_queue);
    queue_action(ACTION_HOME, 1, 1000, 1, 0);
    return res;
}

static mcp_tool_result_t tool_set_trim_cb(const void *args, void *ud) {
    (void)ud;
    cJSON *root = (cJSON *)args;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };

    cJSON *st = cJSON_GetObjectItem(root, "servo_type");
    const char *servo_type = (st && cJSON_IsString(st)) ? st->valuestring : "left_leg";

    cJSON *tv = cJSON_GetObjectItem(root, "trim_value");
    int trim_value = (tv && cJSON_IsNumber(tv)) ? tv->valueint : 0;

    ESP_LOGI(TAG, "设置舵机微调: %s = %d度", servo_type, trim_value);

    settings_t *s = settings_open("otto_trims", true);
    int ll = settings_get_int(s, "left_leg", 0);
    int rl = settings_get_int(s, "right_leg", 0);
    int lf = settings_get_int(s, "left_foot", 0);
    int rf = settings_get_int(s, "right_foot", 0);
    int lh = settings_get_int(s, "left_hand", 0);
    int rh = settings_get_int(s, "right_hand", 0);

    if (strcmp(servo_type, "left_leg") == 0)        { ll = trim_value; settings_set_int(s, "left_leg", ll); }
    else if (strcmp(servo_type, "right_leg") == 0)   { rl = trim_value; settings_set_int(s, "right_leg", rl); }
    else if (strcmp(servo_type, "left_foot") == 0)   { lf = trim_value; settings_set_int(s, "left_foot", lf); }
    else if (strcmp(servo_type, "right_foot") == 0)  { rf = trim_value; settings_set_int(s, "right_foot", rf); }
    else if (strcmp(servo_type, "left_hand") == 0) {
        if (!g_ctrl->has_hands) { settings_close(s); res.is_error = true; res.text = strdup("错误：机器人没有配置手部舵机"); return res; }
        lh = trim_value; settings_set_int(s, "left_hand", lh);
    }
    else if (strcmp(servo_type, "right_hand") == 0) {
        if (!g_ctrl->has_hands) { settings_close(s); res.is_error = true; res.text = strdup("错误：机器人没有配置手部舵机"); return res; }
        rh = trim_value; settings_set_int(s, "right_hand", rh);
    }
    else {
        settings_close(s);
        res.is_error = true;
        res.text = strdup("错误：无效的舵机类型，请使用: left_leg, right_leg, left_foot, right_foot, left_hand, right_hand");
        return res;
    }
    settings_close(s);

    otto_set_trims(g_ctrl->otto, ll, rl, lf, rf, lh, rh);
    queue_action(ACTION_JUMP, 1, 500, 0, 0);

    char buf[128];
    snprintf(buf, sizeof(buf), "舵机 %s 微调设置为 %d 度，已永久保存", servo_type, trim_value);
    res.text = strdup(buf);
    return res;
}

static mcp_tool_result_t tool_get_trims_cb(const void *args, void *ud) {
    (void)args; (void)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };

    settings_t *s = settings_open("otto_trims", false);
    int ll = settings_get_int(s, "left_leg", 0);
    int rl = settings_get_int(s, "right_leg", 0);
    int lf = settings_get_int(s, "left_foot", 0);
    int rf = settings_get_int(s, "right_foot", 0);
    int lh = settings_get_int(s, "left_hand", 0);
    int rh = settings_get_int(s, "right_hand", 0);
    settings_close(s);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"left_leg\":%d,\"right_leg\":%d,\"left_foot\":%d,"
             "\"right_foot\":%d,\"left_hand\":%d,\"right_hand\":%d}",
             ll, rl, lf, rf, lh, rh);
    ESP_LOGI(TAG, "获取微调设置: %s", buf);
    res.text = strdup(buf);
    return res;
}

static mcp_tool_result_t tool_get_status_cb(const void *args, void *ud) {
    (void)args; (void)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    res.text = strdup(g_ctrl->is_action_in_progress ? "moving" : "idle");
    return res;
}

/* ── MCP tool registration ─────────────────────────────── */
static void register_mcp_tools(void) {
    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    ESP_LOGI(TAG, "开始注册MCP工具...");

    /* self.otto.action */
    {
        static const mcp_tool_param_t params[] = {
            { "action",    MCP_PARAM_TYPE_STRING  },
            { "steps",     MCP_PARAM_TYPE_INTEGER },
            { "speed",     MCP_PARAM_TYPE_INTEGER },
            { "direction", MCP_PARAM_TYPE_INTEGER },
            { "amount",    MCP_PARAM_TYPE_INTEGER },
            { "arm_swing", MCP_PARAM_TYPE_INTEGER },
        };
        mcp_server_add_tool_c(mcp, "self.otto.action",
            "执行机器人动作。action: 动作名称；根据动作类型提供相应参数：direction: 方向，1=前进/左转，-1=后退/右转；0=左右同时"
            "steps: 动作步数，1-100；speed: 动作速度，100-3000，数值越小越快；amount: 动作幅度，0-170；arm_swing: 手臂摆动幅度，0-170；"
            "基础动作：walk(行走，需steps/speed/direction/arm_swing)、turn(转身，需steps/speed/direction/arm_swing)、jump(跳跃，需steps/speed)、"
            "swing(摇摆，需steps/speed/amount)、moonwalk(太空步，需steps/speed/direction/amount)、bend(弯曲，需steps/speed/direction)、"
            "shake_leg(摇腿，需steps/speed/direction)、updown(上下运动，需steps/speed/amount)、whirlwind_leg(旋风腿，需steps/speed/amount)；"
            "固定动作：sit(坐下)、showcase(展示动作)、home(复位)；"
            "手部动作(需手部舵机)：hands_up(举手，需speed/direction)、hands_down(放手，需speed/direction)、hand_wave(挥手，需direction)、"
            "windmill(大风车，需steps/speed/amount)、takeoff(起飞，需steps/speed/amount)、fitness(健身，需steps/speed/amount)、"
            "greeting(打招呼，需direction/steps)、shy(害羞，需direction/steps)、radio_calisthenics(广播体操)、magic_circle(爱的魔力转圈圈)",
            params, 6, tool_action_cb, g_ctrl);
    }

    /* self.otto.servo_sequences */
    {
        static const mcp_tool_param_t params[] = {
            { "sequence", MCP_PARAM_TYPE_STRING },
        };
        mcp_server_add_tool_c(mcp, "self.otto.servo_sequences",
            "AI自定义动作编程（即兴动作）。支持分段发送序列：超过5个序列建议AI可以连续多次调用此工具，每次发送一个短序列，系统会自动排队按顺序执行。支持普通移动和振荡器两种模式。"
            "机器人结构：双手可上下摆动，双腿可内收外展，双脚可上下翻转。"
            "舵机说明："
            "ll(左腿)：内收外展，0度=完全外展，90度=中立，180度=完全内收；"
            "rl(右腿)：内收外展，0度=完全内收，90度=中立，180度=完全外展；"
            "lf(左脚)：上下翻转，0度=完全向上，90度=水平，180度=完全向下；"
            "rf(右脚)：上下翻转，0度=完全向下，90度=水平，180度=完全向上；"
            "lh(左手)：上下摆动，0度=完全向下，90度=水平，180度=完全向上；"
            "rh(右手)：上下摆动，0度=完全向上，90度=水平，180度=完全向下；"
            "sequence: 单个序列对象，包含'a'动作数组，顶层可选参数："
            "'d'(序列执行完成后延迟毫秒数，用于序列之间的停顿)。"
            "每个动作对象包含："
            "普通模式：'s'舵机位置对象(键名：ll/rl/lf/rf/lh/rh，值：0-180度)，'v'移动速度100-3000毫秒(默认1000)，'d'动作后延迟毫秒数(默认0)；"
            "振荡模式：'osc'振荡器对象，包含'a'振幅对象(各舵机振幅10-90度，默认20度)，'o'中心角度对象(各舵机振荡中心绝对角度0-180度，默认90度)，'ph'相位差对象(各舵机相位差，度，0-360度，默认0度)，'p'周期100-3000毫秒(默认500)，'c'周期数0.1-20.0(默认5.0)；"
            "使用方式：AI可以连续多次调用此工具，每次发送一个序列，系统会自动排队按顺序执行。"
            "重要说明：左右腿脚震荡的时候，有一只脚必须在90度，否则会损坏机器人，如果发送多个序列（序列数>1），完成所有序列后需要复位时，AI应该最后单独调用self.otto.home工具进行复位，不要在序列中设置复位参数。"
            "普通模式示例：发送3个序列，最后调用复位："
            "第1次调用{\"sequence\":\"{\\\"a\\\":[{\\\"s\\\":{\\\"ll\\\":100},\\\"v\\\":1000}],\\\"d\\\":500}\"}，"
            "第2次调用{\"sequence\":\"{\\\"a\\\":[{\\\"s\\\":{\\\"ll\\\":90},\\\"v\\\":800}],\\\"d\\\":500}\"}，"
            "第3次调用{\"sequence\":\"{\\\"a\\\":[{\\\"s\\\":{\\\"ll\\\":80},\\\"v\\\":800}]}\"}，"
            "最后调用self.otto.home工具进行复位。"
            "振荡器模式示例："
            "示例1-双臂同步摆动：{\"sequence\":\"{\\\"a\\\":[{\\\"osc\\\":{\\\"a\\\":{\\\"lh\\\":30,\\\"rh\\\":30},\\\"o\\\":{\\\"lh\\\":90,\\\"rh\\\":-90},\\\"p\\\":500,\\\"c\\\":5.0}}],\\\"d\\\":0}\"}；"
            "示例2-双腿交替振荡（波浪效果）：{\"sequence\":\"{\\\"a\\\":[{\\\"osc\\\":{\\\"a\\\":{\\\"ll\\\":20,\\\"rl\\\":20},\\\"o\\\":{\\\"ll\\\":90,\\\"rl\\\":-90},\\\"ph\\\":{\\\"rl\\\":180},\\\"p\\\":600,\\\"c\\\":3.0}}],\\\"d\\\":0}\"}；"
            "示例3-单腿振荡配合固定脚（安全）：{\"sequence\":\"{\\\"a\\\":[{\\\"osc\\\":{\\\"a\\\":{\\\"ll\\\":45},\\\"o\\\":{\\\"ll\\\":90,\\\"lf\\\":90},\\\"p\\\":400,\\\"c\\\":4.0}}],\\\"d\\\":0}\"}；"
            "示例4-复杂多舵机振荡（手和腿）：{\"sequence\":\"{\\\"a\\\":[{\\\"osc\\\":{\\\"a\\\":{\\\"lh\\\":25,\\\"rh\\\":25,\\\"ll\\\":15},\\\"o\\\":{\\\"lh\\\":90,\\\"rh\\\":90,\\\"ll\\\":90,\\\"lf\\\":90},\\\"ph\\\":{\\\"rh\\\":180},\\\"p\\\":800,\\\"c\\\":6.0}}],\\\"d\\\":500}\"}；"
            "示例5-快速摇摆：{\"sequence\":\"{\\\"a\\\":[{\\\"osc\\\":{\\\"a\\\":{\\\"ll\\\":30,\\\"rl\\\":30},\\\"o\\\":{\\\"ll\\\":90,\\\"rl\\\":90},\\\"ph\\\":{\\\"rl\\\":180},\\\"p\\\":300,\\\"c\\\":10.0}}],\\\"d\\\":0}\"}。",
            params, 1, tool_servo_sequences_cb, g_ctrl);
    }

    /* self.otto.stop */
    mcp_server_add_tool_c(mcp, "self.otto.stop",
        "立即停止所有动作并复位", NULL, 0, tool_stop_cb, g_ctrl);

    /* self.otto.set_trim */
    {
        static const mcp_tool_param_t params[] = {
            { "servo_type", MCP_PARAM_TYPE_STRING  },
            { "trim_value", MCP_PARAM_TYPE_INTEGER },
        };
        mcp_server_add_tool_c(mcp, "self.otto.set_trim",
            "校准单个舵机位置。设置指定舵机的微调参数以调整机器人的初始站立姿态，设置将永久保存。"
            "servo_type: 舵机类型(left_leg/right_leg/left_foot/right_foot/left_hand/right_hand); "
            "trim_value: 微调值(-50到50度)",
            params, 2, tool_set_trim_cb, g_ctrl);
    }

    /* self.otto.get_trims */
    mcp_server_add_tool_c(mcp, "self.otto.get_trims",
        "获取当前的舵机微调设置", NULL, 0, tool_get_trims_cb, g_ctrl);

    /* self.otto.get_status */
    mcp_server_add_tool_c(mcp, "self.otto.get_status",
        "获取机器人状态，返回 moving 或 idle", NULL, 0, tool_get_status_cb, g_ctrl);

    ESP_LOGI(TAG, "MCP工具注册完成");
}

/* ── Public API ────────────────────────────────────────── */
void InitializeOttoController(int ll, int rl, int lf, int rf, int lh, int rh) {
    if (g_ctrl) return;

    g_ctrl = (otto_controller_t *)calloc(1, sizeof(otto_controller_t));
    g_ctrl->otto = otto_create();
    otto_init(g_ctrl->otto, ll, rl, lf, rf, lh, rh);

    g_ctrl->has_hands = (lh != -1 && rh != -1);
    ESP_LOGI(TAG, "Otto机器人初始化%s手部舵机", g_ctrl->has_hands ? "带" : "不带");
    ESP_LOGI(TAG, "舵机引脚配置: LL=%d, RL=%d, LF=%d, RF=%d, LH=%d, RH=%d",
             ll, rl, lf, rf, lh, rh);

    load_trims_from_nvs();

    g_ctrl->action_queue = xQueueCreate(10, sizeof(otto_action_params_t));
    queue_action(ACTION_HOME, 1, 1000, 1, 0);

    register_mcp_tools();
    ESP_LOGI(TAG, "Otto控制器已初始化并注册MCP工具");
}
