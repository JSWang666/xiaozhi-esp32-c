#ifndef PRESS_TO_TALK_MCP_TOOL_H
#define PRESS_TO_TALK_MCP_TOOL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct press_to_talk_mcp_tool press_to_talk_mcp_tool_t;

press_to_talk_mcp_tool_t *press_to_talk_mcp_tool_create(void);
void press_to_talk_mcp_tool_destroy(press_to_talk_mcp_tool_t *tool);
void press_to_talk_mcp_tool_initialize(press_to_talk_mcp_tool_t *tool);
bool press_to_talk_mcp_tool_is_enabled(const press_to_talk_mcp_tool_t *tool);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
class PressToTalkMcpTool {
public:
    PressToTalkMcpTool() : handle_(press_to_talk_mcp_tool_create()) {}
    ~PressToTalkMcpTool() { press_to_talk_mcp_tool_destroy(handle_); }

    void Initialize() { press_to_talk_mcp_tool_initialize(handle_); }
    bool IsPressToTalkEnabled() const { return press_to_talk_mcp_tool_is_enabled(handle_); }

private:
    PressToTalkMcpTool(const PressToTalkMcpTool&) = delete;
    PressToTalkMcpTool& operator=(const PressToTalkMcpTool&) = delete;
    press_to_talk_mcp_tool_t *handle_;
};
#endif

#endif /* PRESS_TO_TALK_MCP_TOOL_H */
