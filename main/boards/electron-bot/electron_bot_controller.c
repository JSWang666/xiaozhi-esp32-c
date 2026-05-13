#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c_api/mcp_server_c_api.h"
#include "config.h"
#include "settings.h"

#define TAG "ElectronBotController"

#define RIGHT_PITCH 0
#define RIGHT_ROLL 1
#define LEFT_PITCH 2
#define LEFT_ROLL 3
#define BODY 4
#define HEAD 5
#define SERVO_COUNT 6

/* ---------- oscillator_t (must match oscillator.c) ---------- */
#include <driver/ledc.h>
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

/* ---------- otto_t (must match movements.c) ---------- */
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

extern void otto_init(otto_t *o, int rp, int rr, int lp, int lr, int body, int head);
extern void otto_attach_servos(otto_t *o);
extern void otto_set_trims(otto_t *o, int rp, int rr, int lp, int lr, int body, int head);
extern void otto_home(otto_t *o, bool hands_down);
extern void otto_hand_action(otto_t *o, int action, int times, int amount, int period);
extern void otto_body_action(otto_t *o, int action, int times, int amount, int period);
extern void otto_head_action(otto_t *o, int action, int times, int amount, int period);

/* ---------- action parameters ---------- */
typedef struct {
    int action_type;
    int steps;
    int speed;
    int direction;
    int amount;
} action_params_t;

enum {
    ACTION_HAND_LEFT_UP = 1,
    ACTION_HAND_RIGHT_UP = 2,
    ACTION_HAND_BOTH_UP = 3,
    ACTION_HAND_LEFT_DOWN = 4,
    ACTION_HAND_RIGHT_DOWN = 5,
    ACTION_HAND_BOTH_DOWN = 6,
    ACTION_HAND_LEFT_WAVE = 7,
    ACTION_HAND_RIGHT_WAVE = 8,
    ACTION_HAND_BOTH_WAVE = 9,
    ACTION_HAND_LEFT_FLAP = 10,
    ACTION_HAND_RIGHT_FLAP = 11,
    ACTION_HAND_BOTH_FLAP = 12,
    ACTION_BODY_TURN_LEFT = 13,
    ACTION_BODY_TURN_RIGHT = 14,
    ACTION_BODY_TURN_CENTER = 15,
    ACTION_HEAD_UP = 16,
    ACTION_HEAD_DOWN = 17,
    ACTION_HEAD_NOD_ONCE = 18,
    ACTION_HEAD_CENTER = 19,
    ACTION_HEAD_NOD_REPEAT = 20,
    ACTION_HOME = 21,
};

/* ---------- controller ---------- */
typedef struct {
    otto_t bot;
    TaskHandle_t action_task;
    QueueHandle_t action_queue;
    bool is_action_in_progress;
} electron_bot_controller_t;

static void action_task_fn(void *arg);
static void queue_action(electron_bot_controller_t *ctrl,
                         int action_type, int steps, int speed,
                         int direction, int amount);
static void start_action_task_if_needed(electron_bot_controller_t *ctrl);
static void load_trims_from_nvs(electron_bot_controller_t *ctrl);
static void register_mcp_tools(electron_bot_controller_t *ctrl);

/* ------------------------------------------------------------------ */
static void action_task_fn(void *arg)
{
    electron_bot_controller_t *ctrl = (electron_bot_controller_t *)arg;
    action_params_t params;
    otto_attach_servos(&ctrl->bot);

    while (true) {
        if (xQueueReceive(ctrl->action_queue, &params, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "Action: %d", params.action_type);
            ctrl->is_action_in_progress = true;

            if (params.action_type >= ACTION_HAND_LEFT_UP &&
                params.action_type <= ACTION_HAND_BOTH_FLAP) {
                otto_hand_action(&ctrl->bot, params.action_type,
                                 params.steps, params.amount, params.speed);
            } else if (params.action_type >= ACTION_BODY_TURN_LEFT &&
                       params.action_type <= ACTION_BODY_TURN_CENTER) {
                int dir = params.action_type - ACTION_BODY_TURN_LEFT + 1;
                otto_body_action(&ctrl->bot, dir,
                                 params.steps, params.amount, params.speed);
            } else if (params.action_type >= ACTION_HEAD_UP &&
                       params.action_type <= ACTION_HEAD_NOD_REPEAT) {
                int act = params.action_type - ACTION_HEAD_UP + 1;
                otto_head_action(&ctrl->bot, act,
                                 params.steps, params.amount, params.speed);
            } else if (params.action_type == ACTION_HOME) {
                otto_home(&ctrl->bot, true);
            }
            ctrl->is_action_in_progress = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void queue_action(electron_bot_controller_t *ctrl,
                         int action_type, int steps, int speed,
                         int direction, int amount)
{
    ESP_LOGI(TAG, "Queue: type=%d steps=%d speed=%d dir=%d amount=%d",
             action_type, steps, speed, direction, amount);
    action_params_t params = {action_type, steps, speed, direction, amount};
    xQueueSend(ctrl->action_queue, &params, portMAX_DELAY);
    start_action_task_if_needed(ctrl);
}

static void start_action_task_if_needed(electron_bot_controller_t *ctrl)
{
    if (ctrl->action_task == NULL) {
        xTaskCreate(action_task_fn, "electron_bot_action",
                    1024 * 4, ctrl, configMAX_PRIORITIES - 1,
                    &ctrl->action_task);
    }
}

static void load_trims_from_nvs(electron_bot_controller_t *ctrl)
{
    settings_t *s = settings_open("electron_trims", false);
    int rp = settings_get_int(s, "right_pitch", 0);
    int rr = settings_get_int(s, "right_roll", 0);
    int lp = settings_get_int(s, "left_pitch", 0);
    int lr = settings_get_int(s, "left_roll", 0);
    int bd = settings_get_int(s, "body", 0);
    int hd = settings_get_int(s, "head", 0);
    settings_close(s);
    otto_set_trims(&ctrl->bot, rp, rr, lp, lr, bd, hd);
}

/* ---- MCP tool callbacks ---- */

static mcp_tool_result_t hand_action_cb(const void *args, void *ud)
{
    electron_bot_controller_t *ctrl = (electron_bot_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int action_type = cJSON_GetObjectItem(json, "action")->valueint;
    int hand_type   = cJSON_GetObjectItem(json, "hand")->valueint;
    int steps       = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed       = cJSON_GetObjectItem(json, "speed")->valueint;
    int amount      = cJSON_GetObjectItem(json, "amount")->valueint;

    int base_action;
    switch (action_type) {
    case 1: base_action = ACTION_HAND_LEFT_UP;   break;
    case 2: base_action = ACTION_HAND_LEFT_DOWN;  amount = 0; break;
    case 3: base_action = ACTION_HAND_LEFT_WAVE;  amount = 0; break;
    case 4: base_action = ACTION_HAND_LEFT_FLAP;  amount = 0; break;
    default: base_action = ACTION_HAND_LEFT_UP;   break;
    }
    int action_id = base_action + (hand_type - 1);
    queue_action(ctrl, action_id, steps, speed, 0, amount);

    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t body_turn_cb(const void *args, void *ud)
{
    electron_bot_controller_t *ctrl = (electron_bot_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps     = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    int amount    = cJSON_GetObjectItem(json, "angle")->valueint;

    int action;
    switch (direction) {
    case 1:  action = ACTION_BODY_TURN_LEFT;   break;
    case 2:  action = ACTION_BODY_TURN_RIGHT;  break;
    case 3:  action = ACTION_BODY_TURN_CENTER; break;
    default: action = ACTION_BODY_TURN_LEFT;   break;
    }
    queue_action(ctrl, action, steps, speed, 0, amount);

    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t head_move_cb(const void *args, void *ud)
{
    electron_bot_controller_t *ctrl = (electron_bot_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int action_num = cJSON_GetObjectItem(json, "action")->valueint;
    int steps      = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed      = cJSON_GetObjectItem(json, "speed")->valueint;
    int amount     = cJSON_GetObjectItem(json, "angle")->valueint;
    int action     = ACTION_HEAD_UP + (action_num - 1);

    queue_action(ctrl, action, steps, speed, 0, amount);

    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t stop_cb(const void *args, void *ud)
{
    electron_bot_controller_t *ctrl = (electron_bot_controller_t *)ud;
    (void)args;
    xQueueReset(ctrl->action_queue);
    ctrl->is_action_in_progress = false;
    queue_action(ctrl, ACTION_HOME, 1, 1000, 0, 0);

    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t get_status_cb(const void *args, void *ud)
{
    electron_bot_controller_t *ctrl = (electron_bot_controller_t *)ud;
    (void)args;
    mcp_tool_result_t res = {.is_error = false};
    res.text = strdup(ctrl->is_action_in_progress ? "moving" : "idle");
    return res;
}

static mcp_tool_result_t set_trim_cb(const void *args, void *ud)
{
    electron_bot_controller_t *ctrl = (electron_bot_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    const char *servo_type = cJSON_GetObjectItem(json, "servo_type")->valuestring;
    int trim_value = cJSON_GetObjectItem(json, "trim_value")->valueint;

    ESP_LOGI(TAG, "Set trim: %s = %d", servo_type, trim_value);

    settings_t *s = settings_open("electron_trims", true);
    int rp = settings_get_int(s, "right_pitch", 0);
    int rr = settings_get_int(s, "right_roll", 0);
    int lp = settings_get_int(s, "left_pitch", 0);
    int lr = settings_get_int(s, "left_roll", 0);
    int bd = settings_get_int(s, "body", 0);
    int hd = settings_get_int(s, "head", 0);

    const char *err = NULL;
    if (strcmp(servo_type, "right_pitch") == 0) {
        rp = trim_value; settings_set_int(s, "right_pitch", rp);
    } else if (strcmp(servo_type, "right_roll") == 0) {
        rr = trim_value; settings_set_int(s, "right_roll", rr);
    } else if (strcmp(servo_type, "left_pitch") == 0) {
        lp = trim_value; settings_set_int(s, "left_pitch", lp);
    } else if (strcmp(servo_type, "left_roll") == 0) {
        lr = trim_value; settings_set_int(s, "left_roll", lr);
    } else if (strcmp(servo_type, "body") == 0) {
        bd = trim_value; settings_set_int(s, "body", bd);
    } else if (strcmp(servo_type, "head") == 0) {
        hd = trim_value; settings_set_int(s, "head", hd);
    } else {
        err = "Invalid servo_type";
    }
    settings_close(s);

    if (err) {
        mcp_tool_result_t res = {.is_error = true,
            .text = strdup("Invalid servo_type, use: right_pitch, right_roll, "
                           "left_pitch, left_roll, body, head")};
        return res;
    }

    otto_set_trims(&ctrl->bot, rp, rr, lp, lr, bd, hd);
    queue_action(ctrl, ACTION_HOME, 1, 500, 0, 0);

    char buf[128];
    snprintf(buf, sizeof(buf), "Servo %s trim set to %d, saved", servo_type, trim_value);
    mcp_tool_result_t res = {.is_error = false, .text = strdup(buf)};
    return res;
}

static mcp_tool_result_t get_trims_cb(const void *args, void *ud)
{
    (void)ud;
    (void)args;
    settings_t *s = settings_open("electron_trims", false);
    int rp = settings_get_int(s, "right_pitch", 0);
    int rr = settings_get_int(s, "right_roll", 0);
    int lp = settings_get_int(s, "left_pitch", 0);
    int lr = settings_get_int(s, "left_roll", 0);
    int bd = settings_get_int(s, "body", 0);
    int hd = settings_get_int(s, "head", 0);
    settings_close(s);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"right_pitch\":%d,\"right_roll\":%d,\"left_pitch\":%d,"
        "\"left_roll\":%d,\"body\":%d,\"head\":%d}",
        rp, rr, lp, lr, bd, hd);

    ESP_LOGI(TAG, "Get trims: %s", buf);
    mcp_tool_result_t res = {.is_error = false, .text = strdup(buf)};
    return res;
}

static mcp_tool_result_t get_battery_cb(const void *args, void *ud)
{
    (void)ud;
    (void)args;
    mcp_tool_result_t res = {.is_error = false,
        .text = strdup("{\"level\":0,\"charging\":false}")};
    return res;
}

/* ---- register all tools ---- */
static void register_mcp_tools(electron_bot_controller_t *ctrl)
{
    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    ESP_LOGI(TAG, "Registering Electron Bot MCP tools...");

    {
        static const mcp_tool_param_t params[] = {
            {"action", MCP_PARAM_TYPE_INTEGER},
            {"hand",   MCP_PARAM_TYPE_INTEGER},
            {"steps",  MCP_PARAM_TYPE_INTEGER},
            {"speed",  MCP_PARAM_TYPE_INTEGER},
            {"amount", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.electron.hand_action",
            "Hand action. action: 1=raise, 2=lower, 3=wave, 4=flap; "
            "hand: 1=left, 2=right, 3=both; steps: 1-10; "
            "speed: 500-1500; amount: 10-50",
            params, 5, hand_action_cb, ctrl);
    }
    {
        static const mcp_tool_param_t params[] = {
            {"steps",     MCP_PARAM_TYPE_INTEGER},
            {"speed",     MCP_PARAM_TYPE_INTEGER},
            {"direction", MCP_PARAM_TYPE_INTEGER},
            {"angle",     MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.electron.body_turn",
            "Body turn. steps: 1-10; speed: 500-1500; "
            "direction: 1=left, 2=right, 3=center; angle: 0-90",
            params, 4, body_turn_cb, ctrl);
    }
    {
        static const mcp_tool_param_t params[] = {
            {"action", MCP_PARAM_TYPE_INTEGER},
            {"steps",  MCP_PARAM_TYPE_INTEGER},
            {"speed",  MCP_PARAM_TYPE_INTEGER},
            {"angle",  MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.electron.head_move",
            "Head move. action: 1=up, 2=down, 3=nod, 4=center, 5=nod repeat; "
            "steps: 1-10; speed: 500-1500; angle: 1-15",
            params, 4, head_move_cb, ctrl);
    }

    mcp_server_add_tool_c(mcp, "self.electron.stop",
        "Stop immediately", NULL, 0, stop_cb, ctrl);

    mcp_server_add_tool_c(mcp, "self.electron.get_status",
        "Get robot status (moving or idle)", NULL, 0, get_status_cb, ctrl);

    {
        static const mcp_tool_param_t params[] = {
            {"servo_type", MCP_PARAM_TYPE_STRING},
            {"trim_value", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.electron.set_trim",
            "Calibrate servo trim. servo_type: right_pitch/right_roll/"
            "left_pitch/left_roll/body/head; trim_value: -30..30",
            params, 2, set_trim_cb, ctrl);
    }

    mcp_server_add_tool_c(mcp, "self.electron.get_trims",
        "Get current servo trims", NULL, 0, get_trims_cb, ctrl);

    mcp_server_add_tool_c(mcp, "self.battery.get_level",
        "Get battery level and charge state", NULL, 0, get_battery_cb, ctrl);

    ESP_LOGI(TAG, "Electron Bot MCP tools registered");
}

/* ---- public entry point ---- */

static electron_bot_controller_t *g_electron_controller = NULL;

void InitializeElectronBotController(void)
{
    if (g_electron_controller != NULL)
        return;

    electron_bot_controller_t *ctrl = calloc(1, sizeof(electron_bot_controller_t));
    if (!ctrl) return;

    otto_init(&ctrl->bot, Right_Pitch_Pin, Right_Roll_Pin,
              Left_Pitch_Pin, Left_Roll_Pin, Body_Pin, Head_Pin);

    load_trims_from_nvs(ctrl);
    ctrl->action_queue = xQueueCreate(10, sizeof(action_params_t));

    queue_action(ctrl, ACTION_HOME, 1, 1000, 0, 0);
    register_mcp_tools(ctrl);

    g_electron_controller = ctrl;
    ESP_LOGI(TAG, "Electron Bot controller initialized");
}
