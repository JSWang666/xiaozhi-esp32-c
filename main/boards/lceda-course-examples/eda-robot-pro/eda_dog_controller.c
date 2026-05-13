/*
 * EDA Robot Dog Controller - MCP protocol version (C port)
 */
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "settings.h"
#include "c_api/mcp_server_c_api.h"

#define TAG "EDARobotDogController"

/* ── opaque dog type (defined in eda_dog_movements.c) ── */
struct eda_robot_dog;
typedef struct eda_robot_dog eda_robot_dog_t;

extern eda_robot_dog_t *eda_dog_create(void);
extern void eda_dog_destroy(eda_robot_dog_t *self);
extern void eda_dog_init(eda_robot_dog_t *self, int lf, int lr, int rf, int rr);
extern void eda_dog_attach_servos(eda_robot_dog_t *self);
extern void eda_dog_set_trims(eda_robot_dog_t *self, int lf, int lr, int rf, int rr);
extern void eda_dog_home(eda_robot_dog_t *self);
extern void eda_dog_walk(eda_robot_dog_t *self, float steps, int period, int dir);
extern void eda_dog_turn(eda_robot_dog_t *self, float steps, int period, int dir);
extern void eda_dog_sit(eda_robot_dog_t *self, int period);
extern void eda_dog_stand(eda_robot_dog_t *self, int period);
extern void eda_dog_stretch(eda_robot_dog_t *self, int period);
extern void eda_dog_shake(eda_robot_dog_t *self, int period);
extern void eda_dog_lift_left_front_leg(eda_robot_dog_t *self, int period, int height);
extern void eda_dog_lift_left_rear_leg(eda_robot_dog_t *self, int period, int height);
extern void eda_dog_lift_right_front_leg(eda_robot_dog_t *self, int period, int height);
extern void eda_dog_lift_right_rear_leg(eda_robot_dog_t *self, int period, int height);

/* ── action types ── */
enum {
    ACTION_WALK = 1,
    ACTION_TURN,
    ACTION_SIT,
    ACTION_STAND,
    ACTION_STRETCH,
    ACTION_SHAKE,
    ACTION_LIFT_LEFT_FRONT,
    ACTION_LIFT_LEFT_REAR,
    ACTION_LIFT_RIGHT_FRONT,
    ACTION_LIFT_RIGHT_REAR,
    ACTION_HOME,
};

typedef struct {
    int action_type;
    int steps;
    int speed;
    int direction;
    int height;
} dog_action_params_t;

typedef struct {
    eda_robot_dog_t *dog;
    TaskHandle_t     action_task;
    QueueHandle_t    action_queue;
    bool             is_action_in_progress;
} eda_dog_controller_t;

/* ── forward declarations ── */
static void start_action_task_if_needed(eda_dog_controller_t *ctrl);
static void queue_action(eda_dog_controller_t *ctrl, int type, int steps,
                         int speed, int direction, int height);
static void register_mcp_tools(eda_dog_controller_t *ctrl);

/* ── action task ── */

static void action_task(void *arg)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)arg;
    dog_action_params_t params;
    eda_dog_attach_servos(ctrl->dog);

    while (true) {
        if (xQueueReceive(ctrl->action_queue, &params,
                          pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "执行动作: %d", params.action_type);
            ctrl->is_action_in_progress = true;

            switch (params.action_type) {
            case ACTION_WALK:
                eda_dog_walk(ctrl->dog, params.steps, params.speed, params.direction);
                break;
            case ACTION_TURN:
                eda_dog_turn(ctrl->dog, params.steps, params.speed, params.direction);
                break;
            case ACTION_SIT:
                eda_dog_sit(ctrl->dog, params.speed);
                break;
            case ACTION_STAND:
                eda_dog_stand(ctrl->dog, params.speed);
                break;
            case ACTION_STRETCH:
                eda_dog_stretch(ctrl->dog, params.speed);
                break;
            case ACTION_SHAKE:
                eda_dog_shake(ctrl->dog, params.speed);
                break;
            case ACTION_LIFT_LEFT_FRONT:
                eda_dog_lift_left_front_leg(ctrl->dog, params.speed, params.height);
                break;
            case ACTION_LIFT_LEFT_REAR:
                eda_dog_lift_left_rear_leg(ctrl->dog, params.speed, params.height);
                break;
            case ACTION_LIFT_RIGHT_FRONT:
                eda_dog_lift_right_front_leg(ctrl->dog, params.speed, params.height);
                break;
            case ACTION_LIFT_RIGHT_REAR:
                eda_dog_lift_right_rear_leg(ctrl->dog, params.speed, params.height);
                break;
            case ACTION_HOME:
                eda_dog_home(ctrl->dog);
                break;
            }

            if (params.action_type != ACTION_HOME &&
                params.action_type != ACTION_SIT) {
                eda_dog_home(ctrl->dog);
            }
            ctrl->is_action_in_progress = false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void start_action_task_if_needed(eda_dog_controller_t *ctrl)
{
    if (ctrl->action_task == NULL) {
        xTaskCreate(action_task, "dog_action", 1024 * 3, ctrl,
                    configMAX_PRIORITIES - 1, &ctrl->action_task);
    }
}

static void queue_action(eda_dog_controller_t *ctrl, int type, int steps,
                         int speed, int direction, int height)
{
    ESP_LOGI(TAG, "动作控制: 类型=%d, 步数=%d, 速度=%d, 方向=%d, 高度=%d",
             type, steps, speed, direction, height);

    dog_action_params_t params = {type, steps, speed, direction, height};
    xQueueSend(ctrl->action_queue, &params, portMAX_DELAY);
    start_action_task_if_needed(ctrl);
}

static void load_trims_from_nvs(eda_dog_controller_t *ctrl)
{
    settings_t *s = settings_open("dog_trims", false);

    int left_front_leg  = settings_get_int(s, "left_front_leg", 0);
    int left_rear_leg   = settings_get_int(s, "left_rear_leg", 0);
    int right_front_leg = settings_get_int(s, "right_front_leg", 0);
    int right_rear_leg  = settings_get_int(s, "right_rear_leg", 0);

    settings_close(s);

    ESP_LOGI(TAG, "从NVS加载微调设置: 左前腿=%d, 左后腿=%d, 右前腿=%d, 右后腿=%d",
             left_front_leg, left_rear_leg, right_front_leg, right_rear_leg);

    eda_dog_set_trims(ctrl->dog, left_front_leg, left_rear_leg,
                      right_front_leg, right_rear_leg);
}

/* ── MCP tool callbacks ── */

static mcp_tool_result_t mcp_dog_walk(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps     = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_WALK, steps, speed, direction, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_turn(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int steps     = cJSON_GetObjectItem(json, "steps")->valueint;
    int speed     = cJSON_GetObjectItem(json, "speed")->valueint;
    int direction = cJSON_GetObjectItem(json, "direction")->valueint;
    queue_action(ctrl, ACTION_TURN, steps, speed, direction, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_sit(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed = cJSON_GetObjectItem(json, "speed")->valueint;
    queue_action(ctrl, ACTION_SIT, 1, speed, 0, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_stand(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed = cJSON_GetObjectItem(json, "speed")->valueint;
    queue_action(ctrl, ACTION_STAND, 1, speed, 0, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_stretch(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed = cJSON_GetObjectItem(json, "speed")->valueint;
    queue_action(ctrl, ACTION_STRETCH, 1, speed, 0, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_shake(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed = cJSON_GetObjectItem(json, "speed")->valueint;
    queue_action(ctrl, ACTION_SHAKE, 1, speed, 0, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_lift_left_front(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed  = cJSON_GetObjectItem(json, "speed")->valueint;
    int height = cJSON_GetObjectItem(json, "height")->valueint;
    queue_action(ctrl, ACTION_LIFT_LEFT_FRONT, 1, speed, 0, height);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_lift_left_rear(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed  = cJSON_GetObjectItem(json, "speed")->valueint;
    int height = cJSON_GetObjectItem(json, "height")->valueint;
    queue_action(ctrl, ACTION_LIFT_LEFT_REAR, 1, speed, 0, height);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_lift_right_front(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed  = cJSON_GetObjectItem(json, "speed")->valueint;
    int height = cJSON_GetObjectItem(json, "height")->valueint;
    queue_action(ctrl, ACTION_LIFT_RIGHT_FRONT, 1, speed, 0, height);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_lift_right_rear(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;
    int speed  = cJSON_GetObjectItem(json, "speed")->valueint;
    int height = cJSON_GetObjectItem(json, "height")->valueint;
    queue_action(ctrl, ACTION_LIFT_RIGHT_REAR, 1, speed, 0, height);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_stop(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    (void)args;

    if (ctrl->action_task != NULL) {
        vTaskDelete(ctrl->action_task);
        ctrl->action_task = NULL;
    }
    ctrl->is_action_in_progress = false;
    xQueueReset(ctrl->action_queue);

    queue_action(ctrl, ACTION_HOME, 1, 1000, 0, 0);
    mcp_tool_result_t res = {.is_error = false, .text = NULL};
    return res;
}

static mcp_tool_result_t mcp_dog_set_trim(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    cJSON *json = (cJSON *)args;

    const char *servo_type = cJSON_GetObjectItem(json, "servo_type")->valuestring;
    int trim_value = cJSON_GetObjectItem(json, "trim_value")->valueint;

    ESP_LOGI(TAG, "设置舵机微调: %s = %d度", servo_type, trim_value);

    settings_t *s = settings_open("dog_trims", true);
    int left_front_leg  = settings_get_int(s, "left_front_leg", 0);
    int left_rear_leg   = settings_get_int(s, "left_rear_leg", 0);
    int right_front_leg = settings_get_int(s, "right_front_leg", 0);
    int right_rear_leg  = settings_get_int(s, "right_rear_leg", 0);

    if (strcmp(servo_type, "left_front_leg") == 0) {
        left_front_leg = trim_value;
        settings_set_int(s, "left_front_leg", left_front_leg);
    } else if (strcmp(servo_type, "left_rear_leg") == 0) {
        left_rear_leg = trim_value;
        settings_set_int(s, "left_rear_leg", left_rear_leg);
    } else if (strcmp(servo_type, "right_front_leg") == 0) {
        right_front_leg = trim_value;
        settings_set_int(s, "right_front_leg", right_front_leg);
    } else if (strcmp(servo_type, "right_rear_leg") == 0) {
        right_rear_leg = trim_value;
        settings_set_int(s, "right_rear_leg", right_rear_leg);
    } else {
        settings_close(s);
        mcp_tool_result_t res = {.is_error = true,
            .text = strdup("错误：无效的舵机类型，请使用: left_front_leg, "
                           "left_rear_leg, right_front_leg, right_rear_leg")};
        return res;
    }

    settings_close(s);

    eda_dog_set_trims(ctrl->dog, left_front_leg, left_rear_leg,
                      right_front_leg, right_rear_leg);

    queue_action(ctrl, ACTION_HOME, 1, 500, 0, 0);

    char buf[128];
    snprintf(buf, sizeof(buf), "舵机 %s 微调设置为 %d 度，已永久保存",
             servo_type, trim_value);
    mcp_tool_result_t res = {.is_error = false, .text = strdup(buf)};
    return res;
}

static mcp_tool_result_t mcp_dog_get_trims(const void *args, void *ud)
{
    (void)args;
    (void)ud;

    settings_t *s = settings_open("dog_trims", false);
    int lf = settings_get_int(s, "left_front_leg", 0);
    int lr = settings_get_int(s, "left_rear_leg", 0);
    int rf = settings_get_int(s, "right_front_leg", 0);
    int rr = settings_get_int(s, "right_rear_leg", 0);
    settings_close(s);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"left_front_leg\":%d,\"left_rear_leg\":%d,"
             "\"right_front_leg\":%d,\"right_rear_leg\":%d}",
             lf, lr, rf, rr);

    ESP_LOGI(TAG, "获取微调设置: %s", buf);
    mcp_tool_result_t res = {.is_error = false, .text = strdup(buf)};
    return res;
}

static mcp_tool_result_t mcp_dog_get_status(const void *args, void *ud)
{
    eda_dog_controller_t *ctrl = (eda_dog_controller_t *)ud;
    (void)args;
    mcp_tool_result_t res = {.is_error = false,
        .text = strdup(ctrl->is_action_in_progress ? "moving" : "idle")};
    return res;
}

/* ── MCP tool registration ── */

static void register_mcp_tools(eda_dog_controller_t *ctrl)
{
    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    ESP_LOGI(TAG, "开始注册MCP工具...");

    /* walk */
    {
        static const mcp_tool_param_t params[] = {
            {"steps",     MCP_PARAM_TYPE_INTEGER},
            {"speed",     MCP_PARAM_TYPE_INTEGER},
            {"direction", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.walk",
            "行走。steps: 行走步数(1-100); speed: 行走速度(500-2000，数值越小越快); "
            "direction: 行走方向(-1=后退, 1=前进)",
            params, 3, mcp_dog_walk, ctrl);
    }

    /* turn */
    {
        static const mcp_tool_param_t params[] = {
            {"steps",     MCP_PARAM_TYPE_INTEGER},
            {"speed",     MCP_PARAM_TYPE_INTEGER},
            {"direction", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.turn",
            "转身。steps: 转身步数(1-100); speed: 转身速度(500-2000，数值越小越快); "
            "direction: 转身方向(1=左转, -1=右转)",
            params, 3, mcp_dog_turn, ctrl);
    }

    /* sit */
    {
        static const mcp_tool_param_t params[] = {
            {"speed", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.sit",
            "坐下。speed: 坐下速度(500-2000，数值越小越快)",
            params, 1, mcp_dog_sit, ctrl);
    }

    /* stand */
    {
        static const mcp_tool_param_t params[] = {
            {"speed", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.stand",
            "站立。speed: 站立速度(500-2000，数值越小越快)",
            params, 1, mcp_dog_stand, ctrl);
    }

    /* stretch */
    {
        static const mcp_tool_param_t params[] = {
            {"speed", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.stretch",
            "伸展。speed: 伸展速度(500-2000，数值越小越快)",
            params, 1, mcp_dog_stretch, ctrl);
    }

    /* shake */
    {
        static const mcp_tool_param_t params[] = {
            {"speed", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.shake",
            "摇摆。speed: 摇摆速度(500-2000，数值越小越快)",
            params, 1, mcp_dog_shake, ctrl);
    }

    /* lift_left_front_leg */
    {
        static const mcp_tool_param_t params[] = {
            {"speed",  MCP_PARAM_TYPE_INTEGER},
            {"height", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.lift_left_front_leg",
            "抬起左前腿。speed: 动作速度(500-2000，数值越小越快); height: 抬起高度(10-90度)",
            params, 2, mcp_dog_lift_left_front, ctrl);
    }

    /* lift_left_rear_leg */
    {
        static const mcp_tool_param_t params[] = {
            {"speed",  MCP_PARAM_TYPE_INTEGER},
            {"height", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.lift_left_rear_leg",
            "抬起左后腿。speed: 动作速度(500-2000，数值越小越快); height: 抬起高度(10-90度)",
            params, 2, mcp_dog_lift_left_rear, ctrl);
    }

    /* lift_right_front_leg */
    {
        static const mcp_tool_param_t params[] = {
            {"speed",  MCP_PARAM_TYPE_INTEGER},
            {"height", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.lift_right_front_leg",
            "抬起右前腿。speed: 动作速度(500-2000，数值越小越快); height: 抬起高度(10-90度)",
            params, 2, mcp_dog_lift_right_front, ctrl);
    }

    /* lift_right_rear_leg */
    {
        static const mcp_tool_param_t params[] = {
            {"speed",  MCP_PARAM_TYPE_INTEGER},
            {"height", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.lift_right_rear_leg",
            "抬起右后腿。speed: 动作速度(500-2000，数值越小越快); height: 抬起高度(10-90度)",
            params, 2, mcp_dog_lift_right_rear, ctrl);
    }

    /* stop */
    mcp_server_add_tool_c(mcp, "self.dog.stop",
        "立即停止", NULL, 0, mcp_dog_stop, ctrl);

    /* set_trim */
    {
        static const mcp_tool_param_t params[] = {
            {"servo_type", MCP_PARAM_TYPE_STRING},
            {"trim_value", MCP_PARAM_TYPE_INTEGER},
        };
        mcp_server_add_tool_c(mcp, "self.dog.set_trim",
            "校准单个舵机位置。设置指定舵机的微调参数以调整机器狗的初始站立姿态，设置将永久保存。"
            "servo_type: 舵机类型(left_front_leg/left_rear_leg/right_front_leg/right_rear_leg); "
            "trim_value: 微调值(-50到50度)",
            params, 2, mcp_dog_set_trim, ctrl);
    }

    /* get_trims */
    mcp_server_add_tool_c(mcp, "self.dog.get_trims",
        "获取当前的舵机微调设置", NULL, 0, mcp_dog_get_trims, ctrl);

    /* get_status */
    mcp_server_add_tool_c(mcp, "self.dog.get_status",
        "获取机器狗状态，返回 moving 或 idle", NULL, 0, mcp_dog_get_status, ctrl);

    ESP_LOGI(TAG, "MCP工具注册完成");
}

/* ── global instance and public entry point ── */

static eda_dog_controller_t *g_dog_controller = NULL;

void InitializeEDARobotDogController(void)
{
    if (g_dog_controller != NULL)
        return;

    g_dog_controller = (eda_dog_controller_t *)calloc(1, sizeof(eda_dog_controller_t));
    if (!g_dog_controller) return;

    g_dog_controller->dog = eda_dog_create();
    if (!g_dog_controller->dog) {
        free(g_dog_controller);
        g_dog_controller = NULL;
        return;
    }

    eda_dog_init(g_dog_controller->dog,
                 LEFT_FRONT_LEG_PIN, LEFT_REAR_LEG_PIN,
                 RIGHT_FRONT_LEG_PIN, RIGHT_REAR_LEG_PIN);

    ESP_LOGI(TAG, "EDA机器狗初始化完成");

    load_trims_from_nvs(g_dog_controller);

    g_dog_controller->action_queue = xQueueCreate(10, sizeof(dog_action_params_t));

    queue_action(g_dog_controller, ACTION_HOME, 1, 1000, 0, 0);

    register_mcp_tools(g_dog_controller);

    ESP_LOGI(TAG, "EDA机器狗控制器已初始化并注册MCP工具");
}
