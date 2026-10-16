// Application state shared by the agent loop (agent.cpp) and the slash
// commands (commands.cpp).
#pragma once
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "lmstudio.hpp"

#define CLIL_VERSION "0.1.0"

struct Settings {
    std::string model;         // id sent as "model" (instance id or model key)
    std::string system_extra;  // -s / /system
    double temperature = -1;   // < 0: model default
    int max_tokens = 0;        // 0: unlimited
    int load_ctx = 0;          // context length for /load and auto-load (0: LM Studio default)
    int timeout_s = 180;       // per bash command
    int max_steps = 40;        // tool rounds per turn
    bool tools = true, yes = false, think = true, quiet = false;
};

class App {
public:
    LMStudio lm;
    Settings cfg;
    std::vector<Json> msgs;  // OpenAI-format conversation, msgs[0] is the system prompt
    std::vector<ModelInfo> models;
    int ctx_len = 0;          // context length of the current model, 0 if unknown
    long long ctx_used = -1;  // tokens in context as of `ctx_used_msgs` messages (from usage)
    size_t ctx_used_msgs = 0;
    std::set<std::string> always_allow;
    std::set<std::string> failed_changes;  // files whose write/edit failed this turn
    int tty_fd = -1;                       // /dev/tty for approvals, -1 if none
    bool interactive = false;              // REPL (status lines, hints)

    // models & conversation
    bool refresh_models(std::string& err);
    const ModelInfo* find_model(const std::string& name, std::string* err = nullptr) const;
    std::string resolve_model(const std::string& name, std::string* err = nullptr) const;  // fuzzy -> exact id
    void select_model(const std::string& id);
    void pick_default_model();
    bool load_model(const std::string& key, int ctx);
    bool ensure_model_ready();
    void reset_conversation();
    std::string system_prompt() const;

    // agent loop (agent.cpp); returns 0 ok, 1 error, 130 interrupted
    int run_turn(const std::string& user_text);
    bool compact(bool automatic);
    long long estimate_tokens() const;

    // slash commands (commands.cpp); returns false when the user wants to quit
    bool command(const std::string& line);
    void complete(const std::string& buf, size_t& word_start, std::vector<std::string>& out);
    void menu(const std::string& word, std::vector<std::pair<std::string, std::string>>& out) const;
    std::string expand_mentions(const std::string& text) const;

private:
    std::string build_body(bool tools, const Json* extra_user = nullptr, int max_tokens = 0) const;
    void fit_context(bool mid_turn, size_t incoming_chars);
    size_t prune_history(size_t keep_recent);
    void drop_oldest();
    std::string exec_tool(const ToolCall& call, bool& stop);
    int approve(const std::string& tool, std::string& feedback);
    size_t tool_cap() const;
};

// Messages to the user on stderr; error() also counts into g_errors (exit status).
extern int g_errors;
void note(const std::string& s);
void warn(const std::string& s);
void error(const std::string& s);
std::string find_lms();

// Reply post-processing (agent.cpp), exposed for tests.
std::string strip_think(const std::string& s);
void extract_text_tool_calls(std::string& content, std::vector<ToolCall>& calls);
