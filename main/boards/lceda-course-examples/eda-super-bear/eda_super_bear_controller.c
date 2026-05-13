/*
    EdaRobot controller – pure C, MCP protocol
*/
#include <cJSON.h>
#include <esp_log.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "config.h"
#include "settings.h"
#include "c_api/mcp_server_c_api.h"

#include "eda_super_bear_movements.c"

#define TAG "EdaSuperBearController"

enum {
    ACTION_WALK = 1,
    ACTION_TURN,
    ACTION_JUMP,
    ACTION_SWING,
    ACTION_MOONWALK,
    ACTION_BEND,
    ACTION_SHAKE_LEG,
    ACTION_UPDOWN,
    ACTION_TIPTOE_SWING,
    ACTION_JITTER,
    ACTION_ASCENDING_TURN,
    ACTION_CRUSAITO,
    ACTION_FLAPPING,
    ACTION_HANDS_UP,
    ACTION_HANDS_DOWN,
    ACTION_HAND_WAVE,
    ACTION_HOME,
};

typedef struct {
    int action_type;
    int steps;
    int speed;
    int direction;
    int amount;
} eda_robot_action_params_t;

typedef struct {
    eda_robot_t robot;
    TaskHandle_t action_task;
    QueueHandle_t action_queue;
    bool has_hands;
    bool is_action_in_progress;
} eda_super_bear_controller_t;

/* ---- forward declarations ---- */
static void queue_action(eda_super_bear_controller_t *ctrl,
                         int action_type, int steps, int speed, int direction, int amount);

/* ---- action task ---- */
static void action_task_fn(void *arg)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)arg;
    eda_robot_action_params_t params;
    eda_robot_attach_servos(&ctrl->robot);

    while (true) {
        if (xQueueReceive(ctrl->action_queue, &params, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "Executing action: %d", params.action_type);
            ctrl->is_action_in_progress = true;

            switch (params.action_type) {
            case ACTION_WALK:
                eda_robot_walk(&ctrl->robot, params.steps, params.speed, params.direction, params.amount);
                break;
            case ACTION_TURN:
                eda_robot_turn(&ctrl->robot, params.steps, params.speed, params.direction, params.amount);
                break;
            case ACTION_JUMP:
                eda_robot_jump(&ctrl->robot, params.steps, params.speed);
                break;
            case ACTION_SWING:
                eda_robot_swing(&ctrl->robot, params.steps, params.speed, params.amount);
                break;
            case ACTION_MOONWALK:
                eda_robot_moonwalker(&ctrl->robot, params.steps, params.speed, params.amount, params.direction);
                break;
            case ACTION_BEND:
                eda_robot_bend(&ctrl->robot, params.steps, params.speed, params.direction);
                break;
            case ACTION_SHAKE_LEG:
                eda_robot_shake_leg(&ctrl->robot, params.steps, params.speed, params.direction);
                break;
            case ACTION_UPDOWN:
                eda_robot_updown(&ctrl->robot, params.steps, params.speed, params.amount);
                break;
            case ACTION_TIPTOE_SWING:
                eda_robot_tiptoe_swing(&ctrl->robot, params.steps, params.speed, params.amount);
                break;
            case ACTION_JITTER:
                eda_robot_jitter(&ctrl->robot, params.steps, params.speed, params.amount);
                break;
            case ACTION_ASCENDING_TURN:
                eda_robot_ascending_turn(&ctrl->robot, params.steps, params.speed, params.amount);
                break;
            case ACTION_CRUSAITO:
                eda_robot_crusaito(&ctrl->robot, params.steps, params.speed, params.amount, params.direction);
                break;
            case ACTION_FLAPPING:
                eda_robot_flapping(&ctrl->robot, params.steps, params.speed, params.amount, params.direction);
                break;
            case ACTION_HANDS_UP:
                if (ctrl->has_hands)
                    eda_robot_hands_up(&ctrl->robot, params.speed, params.direction);
                break;
            case ACTION_HANDS_DOWN:
                if (ctrl->has_hands)
                    eda_robot_hands_down(&ctrl->robot, params.speed, params.direction);
                break;
            case ACTION_HAND_WAVE:
                if (ctrl->has_hands)
                    eda_robot_hand_wave(&ctrl->robot, params.speed, params.direction);
                break;
            case ACTION_HOME:
                eda_robot_home(&ctrl->robot, params.direction == 1);
                break;
            }

            if (params.action_type != ACTION_HOME)
                eda_robot_home(&ctrl->robot, params.action_type < ACTION_HANDS_UP);

            ctrl->is_action_in_progress = false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void start_action_task_if_needed(eda_super_bear_controller_t *ctrl)
{
    if (ctrl->action_task == NULL) {
        xTaskCreate(action_task_fn, "edarobot_action", 1024 * 3, ctrl,
                    configMAX_PRIORITIES - 1, &ctrl->action_task);
    }
}

static void queue_action(eda_super_bear_controller_t *ctrl,
                         int action_type, int steps, int speed, int direction, int amount)
{
    if (action_type >= ACTION_HANDS_UP && action_type <= ACTION_HAND_WAVE && !ctrl->has_hands) {
        ESP_LOGW(TAG, "Hand action requested but no hand servos configured");
        return;
    }
    ESP_LOGI(TAG, "Queue action: type=%d steps=%d speed=%d dir=%d amount=%d",
             action_type, steps, speed, direction, amount);

    eda_robot_action_params_t params = {action_type, steps, speed, direction, amount};
    xQueueSend(ctrl->action_queue, &params, portMAX_DELAY);
    start_action_task_if_needed(ctrl);
}

static void load_trims_from_nvs(eda_super_bear_controller_t *ctrl)
{
    settings_t *s = settings_open("edarobot_trims", false);
    int ll = settings_get_int(s, "left_leg", 0);
    int rl = settings_get_int(s, "right_leg", 0);
    int lf = settings_get_int(s, "left_foot", 0);
    int rf = settings_get_int(s, "right_foot", 0);
    int lh = settings_get_int(s, "left_hand", 0);
    int rh = settings_get_int(s, "right_hand", 0);
    settings_close(s);

    ESP_LOGI(TAG, "Loaded trims: ll=%d rl=%d lf=%d rf=%d lh=%d rh=%d", ll, rl, lf, rf, lh, rh);
    eda_robot_set_trims(&ctrl->robot, ll, rl, lf, rf, lh, rh);
}

/* ====== MCP tool callbacks ====== */

static mcp_tool_result_t walk_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps     = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int arm_swing = cJSON_GetObjectItem(json, "arm_swing")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_WALK, steps, speed, direction, arm_swing);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t turn_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps     = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int arm_swing = cJSON_GetObjectItem(json, "arm_swing")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_TURN, steps, speed, direction, arm_swing);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t jump_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed = cJSON_GetObjectItem(json, "speed")->valueint;
    queue_action(ctrl, ACTION_JUMP, steps, speed, 0, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t swing_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps  = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed  = cJSON_GetObjectItem(json, "speed")->valueint;
    int amount = cJSON_GetObjectItem(json, "amount")->valueint;
    queue_action(ctrl, ACTION_SWING, steps, speed, 0, amount);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t moonwalk_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps     = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    int amount    = cJSON_GetObjectItem(json, "amount")->valueint;
    queue_action(ctrl, ACTION_MOONWALK, steps, speed, direction, amount);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t bend_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps     = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_BEND, steps, speed, direction, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t shake_leg_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps     = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_SHAKE_LEG, steps, speed, direction, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t updown_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps  = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed  = cJSON_GetObjectItem(json, "speed")->valueint;
    int amount = cJSON_GetObjectItem(json, "amount")->valueint;
    queue_action(ctrl, ACTION_UPDOWN, steps, speed, 0, amount);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t hands_up_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_HANDS_UP, 1, speed, direction, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t hands_down_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_HANDS_DOWN, 1, speed, direction, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t hand_wave_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_HAND_WAVE, 1, speed, direction, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t stop_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    if (ctrl->action_task != NULL) {
        vTaskDelete(ctrl->action_task);
        ctrl->action_task = NULL;
    }
    ctrl->is_action_in_progress = false;
    xQueueReset(ctrl->action_queue);
    queue_action(ctrl, ACTION_HOME, 1, 1000, 1, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t set_trim_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    const char *servo_type = cJSON_GetObjectItem(json, "servo_type")->valuestring;
    int trim_value = cJSON_GetObjectItem(json, "trim_value")->valueint;

    ESP_LOGI(TAG, "Set trim: %s = %d", servo_type, trim_value);

    settings_t *s = settings_open("edarobot_trims", true);
    int ll = settings_get_int(s, "left_leg", 0);
    int rl = settings_get_int(s, "right_leg", 0);
    int lf = settings_get_int(s, "left_foot", 0);
    int rf = settings_get_int(s, "right_foot", 0);
    int lh = settings_get_int(s, "left_hand", 0);
    int rh = settings_get_int(s, "right_hand", 0);

    mcp_tool_result_t res = {.is_error = false, .text = NULL};

    if (strcmp(servo_type, "left_leg") == 0) {
        ll = trim_value; settings_set_int(s, "left_leg", ll);
    } else if (strcmp(servo_type, "right_leg") == 0) {
        rl = trim_value; settings_set_int(s, "right_leg", rl);
    } else if (strcmp(servo_type, "left_foot") == 0) {
        lf = trim_value; settings_set_int(s, "left_foot", lf);
    } else if (strcmp(servo_type, "right_foot") == 0) {
        rf = trim_value; settings_set_int(s, "right_foot", rf);
    } else if (strcmp(servo_type, "left_hand") == 0) {
        if (!ctrl->has_hands) {
            settings_close(s);
            res.is_error = true;
            res.text = strdup("Error: no hand servos configured");
            return res;
        }
        lh = trim_value; settings_set_int(s, "left_hand", lh);
    } else if (strcmp(servo_type, "right_hand") == 0) {
        if (!ctrl->has_hands) {
            settings_close(s);
            res.is_error = true;
            res.text = strdup("Error: no hand servos configured");
            return res;
        }
        rh = trim_value; settings_set_int(s, "right_hand", rh);
    } else {
        settings_close(s);
        res.is_error = true;
        res.text = strdup("Error: invalid servo_type");
        return res;
    }
    settings_close(s);

    eda_robot_set_trims(&ctrl->robot, ll, rl, lf, rf, lh, rh);
    queue_action(ctrl, ACTION_JUMP, 1, 500, 0, 0);

    char buf[128];
    snprintf(buf, sizeof(buf), "Servo %s trim set to %d degrees, saved.", servo_type, trim_value);
    res.text = strdup(buf);
    return res;
}

static mcp_tool_result_t get_trims_cb(const void *args, void *ud)
{
    (void)ud;
    settings_t *s = settings_open("edarobot_trims", false);
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
    ESP_LOGI(TAG, "Get trims: %s", buf);

    mcp_tool_result_t res = {.is_error = false, .text = strdup(buf)};
    return res;
}

static mcp_tool_result_t get_status_cb(const void *args, void *ud)
{
    eda_super_bear_controller_t *ctrl = (eda_super_bear_controller_t *)ud;
    mcp_tool_result_t res = {.is_error = false,
        .text = strdup(ctrl->is_action_in_progress ? "moving" : "idle")};
    return res;
}

/* ---- register all MCP tools ---- */
static void register_mcp_tools(eda_super_bear_controller_t *ctrl)
{
    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    ESP_LOGI(TAG, "Registering MCP tools...");

    /* walk */
    {
        static const mcp_tool_param_t p[] = {
            {"steps",     MCP_PARAM_TYPE_INTEGER},
            {"speed",     MCP_PARAM_TYPE_INTEGER},
            {"arm_swing", MCP_PARAM_TYPE_INTEGER},
            {"direction", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.walk_forward",
            "Walk. steps(1-100); speed(500-1500); direction(-1=backward,1=forward); arm_swing(0-170)",
            p, 4, walk_cb, ctrl);
    }
    /* turn */
    {
        static const mcp_tool_param_t p[] = {
            {"steps",     MCP_PARAM_TYPE_INTEGER},
            {"speed",     MCP_PARAM_TYPE_INTEGER},
            {"arm_swing", MCP_PARAM_TYPE_INTEGER},
            {"direction", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.turn_left",
            "Turn. steps(1-100); speed(500-1500); direction(1=left,-1=right); arm_swing(0-170)",
            p, 4, turn_cb, ctrl);
    }
    /* jump */
    {
        static const mcp_tool_param_t p[] = {
            {"steps", MCP_PARAM_TYPE_INTEGER},
            {"speed", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.jump",
            "Jump. steps(1-100); speed(500-1500)",
            p, 2, jump_cb, ctrl);
    }
    /* swing */
    {
        static const mcp_tool_param_t p[] = {
            {"steps",  MCP_PARAM_TYPE_INTEGER},
            {"speed",  MCP_PARAM_TYPE_INTEGER},
            {"amount", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.swing",
            "Swing side to side. steps(1-100); speed(500-1500); amount(0-170)",
            p, 3, swing_cb, ctrl);
    }
    /* moonwalk */
    {
        static const mcp_tool_param_t p[] = {
            {"steps",     MCP_PARAM_TYPE_INTEGER},
            {"speed",     MCP_PARAM_TYPE_INTEGER},
            {"direction", MCP_PARAM_TYPE_INTEGER},
            {"amount",    MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.moonwalk",
            "Moonwalk. steps(1-100); speed(500-1500); direction(1=left,-1=right); amount(0-170)",
            p, 4, moonwalk_cb, ctrl);
    }
    /* bend */
    {
        static const mcp_tool_param_t p[] = {
            {"steps",     MCP_PARAM_TYPE_INTEGER},
            {"speed",     MCP_PARAM_TYPE_INTEGER},
            {"direction", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.bend",
            "Bend body. steps(1-100); speed(500-1500); direction(1=left,-1=right)",
            p, 3, bend_cb, ctrl);
    }
    /* shake_leg */
    {
        static const mcp_tool_param_t p[] = {
            {"steps",     MCP_PARAM_TYPE_INTEGER},
            {"speed",     MCP_PARAM_TYPE_INTEGER},
            {"direction", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.shake_leg",
            "Shake leg. steps(1-100); speed(500-1500); direction(1=left,-1=right)",
            p, 3, shake_leg_cb, ctrl);
    }
    /* updown */
    {
        static const mcp_tool_param_t p[] = {
            {"steps",  MCP_PARAM_TYPE_INTEGER},
            {"speed",  MCP_PARAM_TYPE_INTEGER},
            {"amount", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.updown",
            "Up and down. steps(1-100); speed(500-1500); amount(0-170)",
            p, 3, updown_cb, ctrl);
    }
    /* hand actions (only if hands present) */
    if (ctrl->has_hands) {
        {
            static const mcp_tool_param_t p[] = {
                {"speed",     MCP_PARAM_TYPE_INTEGER},
                {"direction", MCP_PARAM_TYPE_INTEGER},
            };
            mcp_server_add_tool_c(mcp, "self.edarobot.hands_up",
                "Hands up. speed(500-1500); direction(1=left,-1=right,0=both)",
                p, 2, hands_up_cb, ctrl);
        }
        {
            static const mcp_tool_param_t p[] = {
                {"speed",     MCP_PARAM_TYPE_INTEGER},
                {"direction", MCP_PARAM_TYPE_INTEGER},
            };
            mcp_server_add_tool_c(mcp, "self.edarobot.hands_down",
                "Hands down. speed(500-1500); direction(1=left,-1=right,0=both)",
                p, 2, hands_down_cb, ctrl);
        }
        {
            static const mcp_tool_param_t p[] = {
                {"speed",     MCP_PARAM_TYPE_INTEGER},
                {"direction", MCP_PARAM_TYPE_INTEGER},
            };
            mcp_server_add_tool_c(mcp, "self.edarobot.hand_wave",
                "Wave hand. speed(500-1500); direction(1=left,-1=right,0=both)",
                p, 2, hand_wave_cb, ctrl);
        }
    }
    /* stop */
    mcp_server_add_tool_c(mcp, "self.edarobot.stop",
        "Stop immediately", NULL, 0, stop_cb, ctrl);
    /* set_trim */
    {
        static const mcp_tool_param_t p[] = {
            {"servo_type", MCP_PARAM_TYPE_STRING},
            {"trim_value", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.edarobot.set_trim",
            "Set single servo trim. servo_type(left_leg/right_leg/left_foot/right_foot/left_hand/right_hand); trim_value(-50 to 50)",
            p, 2, set_trim_cb, ctrl);
    }
    /* get_trims */
    mcp_server_add_tool_c(mcp, "self.edarobot.get_trims",
        "Get current servo trims", NULL, 0, get_trims_cb, ctrl);
    /* get_status */
    mcp_server_add_tool_c(mcp, "self.edarobot.get_status",
        "Get robot status (moving/idle)", NULL, 0, get_status_cb, ctrl);

    ESP_LOGI(TAG, "MCP tools registered");
}

/* ---- public init function ---- */
static eda_super_bear_controller_t *g_controller = NULL;

void InitializeEdaSuperBearController(void)
{
    if (g_controller != NULL)
        return;

    g_controller = (eda_super_bear_controller_t *)calloc(1, sizeof(*g_controller));
    eda_robot_init_struct(&g_controller->robot);
    eda_robot_init(&g_controller->robot,
                   LEFT_LEG_PIN, RIGHT_LEG_PIN, LEFT_FOOT_PIN, RIGHT_FOOT_PIN,
                   LEFT_HAND_PIN, RIGHT_HAND_PIN);

    g_controller->has_hands = (LEFT_HAND_PIN != -1 && RIGHT_HAND_PIN != -1);
    ESP_LOGI(TAG, "EdaRobot initialized %s hand servos",
             g_controller->has_hands ? "with" : "without");

    load_trims_from_nvs(g_controller);

    g_controller->action_queue = xQueueCreate(10, sizeof(eda_robot_action_params_t));
    queue_action(g_controller, ACTION_HOME, 1, 1000, 1, 0);

    register_mcp_tools(g_controller);
    ESP_LOGI(TAG, "EdaRobot controller initialized");
}
