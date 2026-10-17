// MCP client (stdio servers only), reading the same ~/.lmstudio/mcp.json as LM Studio.
// Tools of connected servers are exposed to the model as "<server>__<tool>".
#pragma once
#include <string>
#include <vector>

#include "json.hpp"
#include "tools.hpp"

struct McpInfo {
    std::string name;
    bool connected = false;
    bool supported = true;  // false: remote (url) server
    size_t tools = 0;
};

std::vector<McpInfo> mcp_list();
bool mcp_connect(const std::string& name, std::string& err);
bool mcp_disconnect(const std::string& name);
void mcp_shutdown();

bool mcp_owns(const std::string& tool);
const Json& mcp_schemas();  // OpenAI tool entries of all connected servers
int mcp_generation();       // bumps whenever the tool set changes
ToolOutput mcp_call(const std::string& tool, const Json& args, size_t cap, int timeout_s);

// Entry point of `clil --mcp-host NAME`: runs the server once and shares it with every clil
// that connects over a unix socket; exits after CLIL_MCP_IDLE seconds (default 300) without clients.
int mcp_host_main(const std::string& name);
