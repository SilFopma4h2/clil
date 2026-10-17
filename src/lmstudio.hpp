// LM Studio client: native REST API (/api/v1, falling back to /api/v0) for
// model management, OpenAI-compatible /v1/chat/completions for chat.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "http.hpp"
#include "json.hpp"

struct ModelInfo {
    std::string key, display, type, arch, quant, format, params;
    long long size = 0;
    int max_ctx = 0;
    int parallel = 0;  // concurrent predictions of the loaded instance (0: unknown)
    bool tools = false, vision = false;
    std::vector<std::pair<std::string, int>> loaded;  // instance id, context length
};

struct ToolCall {
    std::string id, name, args;
};

struct ChatReply {
    std::string content, reasoning;
    std::vector<ToolCall> calls;
    std::string finish, error;
    bool interrupted = false;
    long long prompt_tokens = -1, completion_tokens = -1;
    int deltas = 0;  // streamed pieces, a token estimate when usage is missing
    double ttft = 0, gen_secs = 0;
};

struct ChatEvents {
    std::function<void(const std::string&)> content, reasoning;
    std::function<void(const ToolCall&)> tool_progress;  // a tool call is being streamed
    std::function<void()> tick;
    std::function<void(int)> queued;  // waiting behind N other clil's; 0 when our turn starts
};

struct LoadResult {
    std::string instance;
    double secs = 0;
    int ctx = 0;
};

class LMStudio {
public:
    Endpoint ep;
    int slots = 0;        // requests per server that clil's run at once; 0: what the loaded model allows
    int model_slots = 1;  // that, as reported by LM Studio ("parallel")
    int api = -1;  // detected API: 1 = /api/v1, 0 = /api/v0, 2 = plain OpenAI server, -1 unknown

    bool list_models(std::vector<ModelInfo>& out, std::string& err);
    // Returns 1 on success, 0 if the server lacks the endpoint (older LM Studio), -1 on error.
    int load(const std::string& model, int ctx, LoadResult& res, std::string& err, const std::function<void()>& tick);
    int unload(const std::string& instance, std::string& err);
    int download(const std::string& model, Json& res, std::string& err);
    bool download_status(const std::string& job, Json& res, std::string& err);

    // `body` is a complete /v1/chat/completions request with "stream": true.
    ChatReply chat(const std::string& body, const ChatEvents& ev);

private:
    bool call(const char* method, const std::string& path, const std::string& body, Json& out, std::string& err,
              int* status, int timeout_ms = 15000, const std::function<void()>& tick = nullptr);
};

// "Unexpected endpoint" answers from older servers mean "not supported here".
bool is_missing_endpoint(int status, const Json& body);
