#ifndef MCP_SERVER_C_API_H
#define MCP_SERVER_C_API_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_server_handle mcp_server_handle_t;

mcp_server_handle_t *mcp_server_get_instance(void);
void mcp_server_add_common_tools(mcp_server_handle_t *mcp);
void mcp_server_add_user_only_tools(mcp_server_handle_t *mcp);
void mcp_server_parse_message_json(mcp_server_handle_t *mcp, const void *cjson);
void mcp_server_parse_message_str(mcp_server_handle_t *mcp, const char *message);

typedef enum {
    MCP_PARAM_TYPE_BOOLEAN = 0,
    MCP_PARAM_TYPE_INTEGER = 1,
    MCP_PARAM_TYPE_STRING  = 2,
} mcp_param_type_t;

typedef struct {
    const char     *name;
    mcp_param_type_t type;
} mcp_tool_param_t;

typedef struct {
    bool  is_error;
    char *text;     /* malloc'd; caller frees. NULL treated as "true" on success */
} mcp_tool_result_t;

typedef mcp_tool_result_t (*mcp_tool_callback_t)(const void *args_cjson,
                                                  void *user_data);

void mcp_server_add_tool_c(mcp_server_handle_t *mcp,
                           const char *name,
                           const char *description,
                           const mcp_tool_param_t *params,
                           int param_count,
                           mcp_tool_callback_t callback,
                           void *user_data);

#ifdef __cplusplus
}
#endif

#endif
