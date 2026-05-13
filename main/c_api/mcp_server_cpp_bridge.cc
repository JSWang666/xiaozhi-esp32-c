#include "mcp_server_c_api.h"
#include "mcp_server.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

struct mcp_server_handle {
    McpServer *impl;
};

static mcp_server_handle_t s_mcp_handle;

mcp_server_handle_t *mcp_server_get_instance(void) {
    s_mcp_handle.impl = &McpServer::GetInstance();
    return &s_mcp_handle;
}

void mcp_server_add_common_tools(mcp_server_handle_t *mcp) {
    if (mcp && mcp->impl) mcp->impl->AddCommonTools();
}

void mcp_server_add_user_only_tools(mcp_server_handle_t *mcp) {
    if (mcp && mcp->impl) mcp->impl->AddUserOnlyTools();
}

void mcp_server_parse_message_json(mcp_server_handle_t *mcp, const void *cjson) {
    if (mcp && mcp->impl && cjson) {
        mcp->impl->ParseMessage(static_cast<const cJSON*>(cjson));
    }
}

void mcp_server_parse_message_str(mcp_server_handle_t *mcp, const char *message) {
    if (mcp && mcp->impl && message) {
        mcp->impl->ParseMessage(std::string(message));
    }
}

void mcp_server_add_tool_c(mcp_server_handle_t *mcp,
                           const char *name,
                           const char *description,
                           const mcp_tool_param_t *params,
                           int param_count,
                           mcp_tool_callback_t callback,
                           void *user_data) {
    if (!mcp || !mcp->impl) return;

    PropertyList properties;
    std::vector<std::pair<std::string, mcp_param_type_t>> param_info;
    for (int i = 0; i < param_count; i++) {
        PropertyType pt;
        switch (params[i].type) {
            case MCP_PARAM_TYPE_BOOLEAN: pt = kPropertyTypeBoolean; break;
            case MCP_PARAM_TYPE_INTEGER: pt = kPropertyTypeInteger; break;
            default:                     pt = kPropertyTypeString;  break;
        }
        properties.AddProperty(Property(params[i].name, pt));
        param_info.push_back({params[i].name, params[i].type});
    }

    mcp->impl->AddTool(name, description, properties,
        [callback, user_data, param_info](const PropertyList& props) -> ReturnValue {
            cJSON *args = cJSON_CreateObject();
            for (const auto& p : param_info) {
                const Property& prop = props[p.first];
                switch (p.second) {
                    case MCP_PARAM_TYPE_BOOLEAN:
                        cJSON_AddBoolToObject(args, p.first.c_str(), prop.value<bool>());
                        break;
                    case MCP_PARAM_TYPE_INTEGER:
                        cJSON_AddNumberToObject(args, p.first.c_str(), prop.value<int>());
                        break;
                    case MCP_PARAM_TYPE_STRING:
                        cJSON_AddStringToObject(args, p.first.c_str(),
                                                prop.value<std::string>().c_str());
                        break;
                }
            }

            mcp_tool_result_t result = callback(args, user_data);
            cJSON_Delete(args);

            if (result.is_error) {
                std::string err = result.text ? result.text : "Unknown error";
                free(result.text);
                throw std::runtime_error(err);
            }

            if (result.text) {
                std::string txt(result.text);
                free(result.text);
                return txt;
            }
            return true;
        });
}
