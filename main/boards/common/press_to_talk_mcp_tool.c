#include "press_to_talk_mcp_tool.h"
#include "c_api/mcp_server_c_api.h"
#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <cJSON.h>

static const char *TAG = "PressToTalkMcpTool";

struct press_to_talk_mcp_tool {
    bool press_to_talk_enabled;
};

static void set_press_to_talk_enabled(press_to_talk_mcp_tool_t *tool, bool enabled)
{
    tool->press_to_talk_enabled = enabled;

    settings_t *s = settings_open("vendor", true);
    settings_set_int(s, "press_to_talk", enabled ? 1 : 0);
    settings_close(s);
    ESP_LOGI(TAG, "Press to talk enabled: %d", enabled);
}

static mcp_tool_result_t handle_set_press_to_talk(const void *args_cjson,
                                                   void *user_data)
{
    press_to_talk_mcp_tool_t *tool = (press_to_talk_mcp_tool_t *)user_data;
    const cJSON *args = (const cJSON *)args_cjson;
    mcp_tool_result_t result = { .is_error = false, .text = NULL };

    const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(args, "mode");
    if (!mode_item || !cJSON_IsString(mode_item) || !mode_item->valuestring) {
        result.is_error = true;
        result.text = strdup("Missing or invalid 'mode' parameter");
        return result;
    }

    const char *mode = mode_item->valuestring;

    if (strcmp(mode, "press_to_talk") == 0) {
        set_press_to_talk_enabled(tool, true);
        ESP_LOGI(TAG, "Switched to press to talk mode");
        return result;
    }

    if (strcmp(mode, "click_to_talk") == 0) {
        set_press_to_talk_enabled(tool, false);
        ESP_LOGI(TAG, "Switched to click to talk mode");
        return result;
    }

    result.is_error = true;
    size_t len = strlen("Invalid mode: ") + strlen(mode) + 1;
    result.text = (char *)malloc(len);
    if (result.text) {
        snprintf(result.text, len, "Invalid mode: %s", mode);
    }
    return result;
}

press_to_talk_mcp_tool_t *press_to_talk_mcp_tool_create(void)
{
    press_to_talk_mcp_tool_t *tool = (press_to_talk_mcp_tool_t *)calloc(1, sizeof(*tool));
    return tool;
}

void press_to_talk_mcp_tool_destroy(press_to_talk_mcp_tool_t *tool)
{
    free(tool);
}

void press_to_talk_mcp_tool_initialize(press_to_talk_mcp_tool_t *tool)
{
    if (!tool) return;

    settings_t *s = settings_open("vendor", false);
    tool->press_to_talk_enabled = settings_get_int(s, "press_to_talk", 0) != 0;
    settings_close(s);

    mcp_server_handle_t *mcp = mcp_server_get_instance();

    mcp_tool_param_t params[] = {
        { .name = "mode", .type = MCP_PARAM_TYPE_STRING },
    };

    mcp_server_add_tool_c(mcp,
        "self.set_press_to_talk",
        "Switch between press to talk mode (\xe9\x95\xbf\xe6\x8c\x89\xe8\xaf\xb4\xe8\xaf\x9d) "
        "and click to talk mode (\xe5\x8d\x95\xe5\x87\xbb\xe8\xaf\xb4\xe8\xaf\x9d).\n"
        "The mode can be `press_to_talk` or `click_to_talk`.",
        params, 1,
        handle_set_press_to_talk, tool);

    ESP_LOGI(TAG, "PressToTalkMcpTool initialized, current mode: %s",
             tool->press_to_talk_enabled ? "press_to_talk" : "click_to_talk");
}

bool press_to_talk_mcp_tool_is_enabled(const press_to_talk_mcp_tool_t *tool)
{
    if (!tool) return false;
    return tool->press_to_talk_enabled;
}
