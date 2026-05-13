#include "c_api/mcp_server_c_api.h"
#include "c_api/app_c_api.h"
#include "device_state.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "MCPController"

static bool g_mcp_initialized = false;

static mcp_tool_result_t aec_set_mode(const void *args, void *ud)
{
    (void)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    app_context_t *app = app_get_context();
    if (!app) { res.is_error = true; return res; }

    const char *mode = NULL;
    if (args) {
        cJSON *j = (cJSON *)args;
        cJSON *m = cJSON_GetObjectItem(j, "mode");
        if (m && cJSON_IsString(m)) mode = m->valuestring;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    if (mode && strcmp(mode, "kAecOff") == 0) {
        app_set_aec_mode(app, 0);
        res.text = strdup("{\"success\": true, \"message\": \"AEC off\"}");
    } else {
        app_set_aec_mode(app, 1);
        res.text = strdup("{\"success\": true, \"message\": \"AEC on\"}");
    }
    return res;
}

static mcp_tool_result_t aec_get_mode(const void *args, void *ud)
{
    (void)args; (void)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    app_context_t *app = app_get_context();
    if (!app) { res.is_error = true; return res; }

    int mode = app_get_aec_mode(app);
    if (mode == 0) {
        res.text = strdup("{\"success\": true, \"message\": \"AEC off\"}");
    } else {
        res.text = strdup("{\"success\": true, \"message\": \"AEC on\"}");
    }
    return res;
}

static mcp_tool_result_t esp_restart_tool(const void *args, void *ud)
{
    (void)args; (void)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return res;
}

void InitializeMCPController(void)
{
    if (g_mcp_initialized) return;
    g_mcp_initialized = true;

    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    static const mcp_tool_param_t aec_params[] = {
        { "mode", MCP_PARAM_TYPE_STRING },
    };
    mcp_server_add_tool_c(mcp, "self.AEC.set_mode",
        "Set AEC mode. mode: kAecOff or kAecOnDeviceSide",
        aec_params, 1, aec_set_mode, NULL);

    mcp_server_add_tool_c(mcp, "self.AEC.get_mode",
        "Get AEC mode status.",
        NULL, 0, aec_get_mode, NULL);

    mcp_server_add_tool_c(mcp, "self.res.esp_restart",
        "Restart the device.",
        NULL, 0, esp_restart_tool, NULL);

    ESP_LOGI(TAG, "MCP tools registered");
}
