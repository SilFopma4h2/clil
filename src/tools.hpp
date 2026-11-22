// The agent's tools. Deliberately few and terse: every schema byte is
// re-sent with each request and costs prompt-processing time on a laptop.
#pragma once
#include <string>
#include <vector>

#include "json.hpp"

struct ToolOutput {
    std::string text;                  // what the model sees
    std::string summary;               // one line for the user
    std::vector<std::string> preview;  // a few lines shown to the user
    bool error = false;
};

Json tool_schemas();
bool tool_mutates(const std::string& name);  // needs approval unless --yes
bool tool_known(const std::string& name);

// Key under which "always" is remembered for a call; "" = never remember (destructive command,
// shell chaining, or a write outside the working directory).
std::string tool_always_key(const std::string& name, const Json& args);
bool path_outside_cwd(const std::string& path);

// One-line description of a call, e.g. "bash  ls -la"; plus a preview of a pending change.
std::string tool_title(const std::string& name, const Json& args);
std::vector<std::string> tool_change_preview(const std::string& name, const Json& args);

// `cap` is the maximum number of bytes of output handed back to the model.
ToolOutput tool_run(const std::string& name, const Json& args, size_t cap, int timeout_s);

// Undo for write/edit: files are backed up before they change. Backups are grouped per turn;
// tool_undo() restores the newest group and returns the paths (errors are appended to `err`).
void tool_new_turn();
void tool_forget_reads();  // new conversation: the model has read nothing yet
std::vector<std::string> tool_undo(std::string& err);

// Exposed for tests.
ToolOutput run_edit(const std::string& path, const std::string& old_s, const std::string& new_s, size_t cap = 4000);
ToolOutput run_edits(const std::string& path, const std::vector<std::pair<std::string, std::string>>& list,
                     size_t cap = 4000);
ToolOutput run_insert(const std::string& path, long line, const std::string& text, size_t cap = 4000);
ToolOutput run_read(const std::string& path, long offset, long limit, size_t cap);
ToolOutput run_search(const std::string& pattern, const std::string& path, const std::string& glob, bool icase,
                      size_t cap);
ToolOutput run_bash(const std::string& cmd, size_t cap, int timeout_s);
