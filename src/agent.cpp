// The agent loop: stream a reply, run the tools it asks for, repeat.
#include "app.hpp"
#include "term.hpp"
#include "mcp.hpp"
#include "tools.hpp"
#include "util.hpp"

#include <sys/utsname.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>

void note(const std::string& s) {
    fprintf(stderr, "%s%s%s\n", ce(ansi::dim), s.c_str(), ce(ansi::reset));
}
void warn(const std::string& s) {
    fprintf(stderr, "%s%s%s\n", ce(ansi::yellow), s.c_str(), ce(ansi::reset));
}
int g_errors = 0;

void error(const std::string& s) {
    ++g_errors;
    fprintf(stderr, "%serror:%s %s\n", ce(ansi::red), ce(ansi::reset), s.c_str());
}

static Json message(const char* role, const std::string& content) {
    Json m = Json::object();
    m.set("role", role).set("content", content);
    return m;
}

static const Json& tool_schema_cache() {
    static Json s;
    static int gen = -1;
    if (gen != mcp_generation()) s = tool_schemas(), gen = mcp_generation();
    return s;
}

std::string App::system_prompt() const {
    utsname u;
    uname(&u);
    std::string os = strcmp(u.sysname, "Darwin") == 0 ? "macOS" : u.sysname;
    char date[32];
    time_t t = time(nullptr);
    strftime(date, sizeof date, "%Y-%m-%d", localtime(&t));
    std::string s;
    if (cfg.tools) {
        s = "You are clil, a coding agent working in the user's terminal.\n"
            "OS: " +
            os + " (" + u.machine + "). Working directory: " + cwd() + ". Date: " + date +
            ".\n"
            "Use the tools to inspect and change files and to run commands; never guess file contents.\n"
            "- Use search to find code instead of guessing paths.\n"
            "- Read a file before editing it; copy old_string exactly from the file.\n"
            "- If a tool returns Error, fix the call and retry; never claim success after an error.\n"
            "- Shell commands must be non-interactive.\n"
            "- Be brief. When done, say in 1-3 lines what you did.";
    } else {
        s = "You are clil, a concise assistant in the user's terminal (" + os + ", " + date +
            "). Answer briefly; put code in fenced blocks.";
    }
    for (const char* name : {"AGENTS.md", "CLIL.md"}) {
        std::string body, err;
        if (is_file(name) && read_file(name, body, err) && !trim(body).empty())
            s += std::string("\n\nProject instructions from ") + name + ":\n" + utf8_clip(trim(body), 6000);
    }
    if (!cfg.system_extra.empty()) s += "\n\n" + cfg.system_extra;
    return s;
}

void App::reset_conversation() {
    msgs.clear();
    msgs.push_back(message("system", system_prompt()));
    ctx_used = -1;
    ctx_used_msgs = 0;
}

std::string App::build_body(bool tools, const Json* extra_user, int max_tokens) const {
    std::string b = "{\"model\":";
    json_quote(b, cfg.model);
    b += ",\"messages\":[";
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (i) b += ',';
        msgs[i].dump_to(b);
    }
    if (extra_user) {
        b += ',';
        extra_user->dump_to(b);
    }
    b += "]";
    if (tools) {
        b += ",\"tools\":";
        tool_schema_cache().dump_to(b);
    }
    b += ",\"stream\":true,\"stream_options\":{\"include_usage\":true}";
    if (cfg.temperature >= 0) b += fmt(",\"temperature\":%.3g", cfg.temperature);
    int mt = max_tokens ? max_tokens : cfg.max_tokens;
    if (mt > 0) b += fmt(",\"max_tokens\":%d", mt);
    return b + "}";
}

// ------------------------------------------------------------- context budget

static size_t message_chars(const Json& m) {
    size_t n = m["content"].str().size() + 16;
    for (auto& tc : m["tool_calls"].items())
        n += tc["function"]["arguments"].str().size() + 40;
    return n;
}

long long App::estimate_tokens() const {
    // Real numbers from the last reply plus ~3.5 chars/token for anything newer.
    size_t from = 0;
    double base = cfg.tools ? (double)tool_schema_cache().dump().size() / 3.5 : 0;
    if (ctx_used >= 0 && ctx_used_msgs <= msgs.size()) from = ctx_used_msgs, base = (double)ctx_used;
    size_t chars = 0;
    for (size_t i = from; i < msgs.size(); ++i)
        chars += message_chars(msgs[i]);
    return (long long)(base + (double)chars / 3.5);
}

size_t App::tool_cap() const {
    // Each tool result may use ~20% of the context window (about 0.7 bytes per context token).
    size_t ctx = ctx_len > 0 ? (size_t)ctx_len : 8192;
    size_t cap = ctx * 7 / 10;
    return cap < 2000 ? 2000 : cap > 24000 ? 24000 : cap;
}

// Shrink old tool outputs and file payloads, keeping the newest `keep` tool results intact.
// Happens rarely and all at once, so LM Studio's prompt cache stays valid in between.
size_t App::prune_history(size_t keep) {
    std::vector<size_t> tools;
    for (size_t i = 0; i < msgs.size(); ++i)
        if (msgs[i]["role"].str() == "tool") tools.push_back(i);
    if (tools.size() <= keep) return 0;
    size_t boundary = keep ? tools[tools.size() - keep] : msgs.size();
    size_t n = 0;
    for (size_t i = 1; i < boundary; ++i) {
        Json& m = msgs[i];
        const std::string role = m["role"].str();
        if (role == "tool") {
            const std::string& c = m["content"].str();
            if (c.size() > 400) {
                m.set("content", utf8_clip(c, 200) + fmt("\n[trimmed to save context; was %zu bytes]", c.size()));
                ++n;
            }
        } else if (role == "assistant") {
            Json* tcs = m.find("tool_calls");
            if (!tcs) continue;
            for (auto& tc : tcs->items()) {
                Json* fn = tc.find("function");
                Json* argj = fn ? fn->find("arguments") : nullptr;
                if (!argj || argj->str().size() < 600) continue;
                Json args;
                if (!Json::parse(argj->str(), args)) continue;
                for (auto& mem : args.members())
                    if (mem.second.is_str() && mem.second.str().size() > 300)
                        mem.second = Json(utf8_clip(mem.second.str(), 120) + " [trimmed]");
                *argj = Json(args.dump());
                ++n;
            }
        }
    }
    if (n) ctx_used = -1;
    return n;
}

void App::drop_oldest() {
    // Last resort: forget the oldest exchanges, restarting at a user message.
    size_t limit = ctx_len > 0 ? (size_t)ctx_len * 3 / 5 : 4096;
    while (msgs.size() > 3 && (size_t)estimate_tokens() > limit) {
        size_t cut = 2;
        while (cut < msgs.size() - 1 && msgs[cut]["role"].str() != "user")
            ++cut;
        if (cut >= msgs.size() - 1) break;
        msgs.erase(msgs.begin() + 1, msgs.begin() + (long)cut);
        ctx_used = -1;
    }
}

void App::fit_context(bool mid_turn, size_t incoming) {
    if (ctx_len <= 0) return;
    long long limit = (long long)ctx_len * 3 / 4 - (long long)((double)incoming / 3.5);  // room for reply + input
    long long before = estimate_tokens();
    if (before <= limit) return;
    size_t n = prune_history(3);
    long long after = estimate_tokens();
    if (n)
        note(fmt("context %s/%s: trimmed %zu old tool results (~%s tokens)", human_tokens(before).c_str(),
                 human_ctx(ctx_len).c_str(), n, human_tokens(after).c_str()));
    if (after <= limit) return;
    if (compact(true)) {
        if (mid_turn) msgs.push_back(message("user", "Continue with the task."));
    } else {
        drop_oldest();
    }
}

bool App::compact(bool automatic) {
    if (msgs.size() < 3) {
        if (!automatic) note("nothing to compact yet");
        return false;
    }
    long long before = estimate_tokens();
    // Same prefix as the normal requests (tools included) so LM Studio can reuse its prompt
    // cache; only trim old tool output when the summary request would not fit otherwise.
    if (ctx_len > 0 && before + 1400 > ctx_len) prune_history(1);
    Json ask = message("user",
                       "Summarize our conversation so far so the work can continue in a fresh context. Include the "
                       "goal, key decisions, files read or changed (paths), commands run and their results, and the "
                       "current state and next steps. Use short bullet points. Reply with the summary only.");
    Spinner sp;
    sp.start("compacting conversation");
    ChatEvents ev;
    ev.tick = [&] { sp.tick(); };
    ev.queued = [&](int n) {
        sp.label(n ? fmt("waiting for %s", plural((size_t)n, "other clil").c_str()) : "compacting conversation");
        sp.tick();
    };
    ChatReply rep = lm.chat(build_body(cfg.tools, &ask, 1024), ev);
    sp.stop();
    std::string summary = strip_think(rep.content);
    if (rep.interrupted || !rep.error.empty() || summary.empty()) {
        if (rep.interrupted) g_interrupted = 0;
        warn("compaction failed" + (rep.error.empty() ? std::string() : ": " + rep.error));
        return false;
    }
    Json sys = msgs[0];
    msgs = {sys, message("user", "Summary of our conversation so far:\n" + summary),
            message("assistant", "Understood. I'll continue from there.")};
    ctx_used = -1;
    note(fmt("compacted conversation: ~%s → ~%s tokens", human_tokens(before).c_str(),
             human_tokens(estimate_tokens()).c_str()));
    return true;
}

// ------------------------------------------------------------- streaming output

namespace {
// Prints the reply as it streams: answer text on stdout (code fences tinted),
// reasoning dimmed on stderr, <think> tags routed to the reasoning side.
class Printer {
public:
    Printer(bool show_think, Spinner& sp) : show_(show_think), sp_(sp) {}

    void content(const std::string& s) {
        std::string run;
        for (char ch : s) {
            if (tag_.empty() && ch != '<') {
                run += ch;
                continue;
            }
            tag_ += ch;
            bool open = !in_think_ && prefix_of("<think>"), close = prefix_of("</think>");
            if (open || close) {
                if (tag_ == "<think>" || tag_ == "</think>") {
                    route(run);
                    run.clear();
                    in_think_ = tag_ == "<think>";
                    tag_.clear();
                }
                continue;
            }
            if (tag_.size() > 1 && tag_.back() == '<') {
                run += tag_.substr(0, tag_.size() - 1);
                tag_ = "<";
            } else {
                run += tag_;
                tag_.clear();
            }
        }
        route(run);
    }

    void reasoning(const std::string& s) { think(s); }

    void finish() {
        if (!tag_.empty()) route(tag_), tag_.clear();
        if (line_start_ && !head_.empty()) flush_head();
        if (style_) fputs(co(ansi::reset), stdout), style_ = nullptr;
        if (answered_ && !answer_nl_) fputc('\n', stdout);
        fflush(stdout);
        if (thought_ && show_ && !think_nl_) fputc('\n', stderr);
        fprintf(stderr, "%s", ce(ansi::reset));
    }

    bool answered() const { return answered_; }

private:
    bool show_;
    Spinner& sp_;
    bool in_think_ = false, answered_ = false, thought_ = false, answer_nl_ = true, think_nl_ = true;
    bool line_start_ = true, fence_ = false;
    const char* style_ = nullptr;
    std::string tag_, head_;
    size_t think_chars_ = 0;

    bool prefix_of(const char* tag) const {
        return tag_.size() <= strlen(tag) && strncmp(tag, tag_.c_str(), tag_.size()) == 0;
    }
    void route(const std::string& s) {
        if (s.empty()) return;
        if (in_think_)
            think(s);
        else
            answer(s);
    }
    void think(const std::string& s) {
        thought_ = true;
        if (!show_) {
            think_chars_ += s.size();
            if (!sp_.active()) sp_.start("thinking");
            sp_.label(fmt("thinking (~%s tokens)", human_tokens((long long)think_chars_ / 4).c_str()));
            sp_.tick();
            return;
        }
        sp_.stop();
        fprintf(stderr, "%s%s%s", ce(ansi::dim), s.c_str(), ce(ansi::reset));
        think_nl_ = s.back() == '\n';
    }
    void answer(const std::string& s) {
        std::string t = s;
        if (!answered_) {  // drop leading blank lines (common after </think>)
            size_t k = t.find_first_not_of("\r\n");
            if (k == std::string::npos) return;
            t.erase(0, k);
            sp_.stop();
            if (thought_ && show_) fputs(think_nl_ ? "\n" : "\n\n", stderr);
            answered_ = true;
        }
        sp_.stop();
        for (char ch : t) {
            if (line_start_) {
                head_ += ch;
                if (ch != '\n' && head_.size() < 3) continue;
                flush_head();
                continue;
            }
            fputc(ch, stdout);
            if (ch == '\n') end_line();
        }
        answer_nl_ = t.back() == '\n';
        fflush(stdout);
    }
    void flush_head() {
        bool is_fence = head_.compare(0, 3, "```") == 0;
        if (is_fence) fence_ = !fence_;
        style_ = is_fence ? ansi::dim : fence_ ? ansi::code : head_[0] == '#' ? ansi::bold : nullptr;
        if (style_) fputs(co(style_), stdout);
        fwrite(head_.data(), 1, head_.size(), stdout);
        line_start_ = false;
        if (head_.back() == '\n') end_line();
        head_.clear();
    }
    void end_line() {
        if (style_) fputs(co(ansi::reset), stdout), style_ = nullptr;
        line_start_ = true;
    }
};

}  // namespace

std::string strip_think(const std::string& s) {
    std::string out = s;
    size_t close = out.find("</think>");
    size_t open = out.find("<think>");
    if (close != std::string::npos && (open == std::string::npos || open > close))  // template opened it
        out.erase(0, close + 8);
    for (;;) {
        open = out.find("<think>");
        if (open == std::string::npos) break;
        close = out.find("</think>", open);
        out.erase(open, close == std::string::npos ? std::string::npos : close + 8 - open);
    }
    return trim(out);
}

// Some models print tool calls as text instead of using the API field.
void extract_text_tool_calls(std::string& content, std::vector<ToolCall>& calls) {
    static const char* tags[][2] = {{"<tool_call>", "</tool_call>"}, {"[TOOL_REQUEST]", "[END_TOOL_REQUEST]"}};
    for (auto& t : tags) {
        size_t a;
        while ((a = content.find(t[0])) != std::string::npos) {
            size_t ol = strlen(t[0]), b = content.find(t[1], a + ol);
            std::string inner = trim(content.substr(a + ol, (b == std::string::npos ? content.size() : b) - a - ol));
            Json j;
            if (!Json::parse(inner, j) || !j["name"].is_str() || !tool_known(j["name"].str())) return;
            const Json& args = j.has("arguments") ? j["arguments"] : j["parameters"];
            ToolCall c;
            c.name = j["name"].str();
            c.args = args.is_str() ? args.str() : args.is_obj() ? args.dump() : "{}";
            calls.push_back(c);
            content.erase(a, b == std::string::npos ? std::string::npos : b + strlen(t[1]) - a);
        }
    }
    content = trim(content);
}

namespace {
std::string new_call_id() {
    // Mistral templates insist on 9 alphanumeric characters.
    static unsigned long long x = (unsigned long long)time(nullptr) * 2654435761u ^ (unsigned long long)getpid();
    static const char al[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string id;
    for (int i = 0; i < 9; ++i) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        id += al[x % 62];
    }
    return id;
}
}  // namespace

// ------------------------------------------------------------- tools

int App::approve(const std::string& tool, std::string& feedback) {
    if (cfg.yes || always_allow.count(tool)) return 1;
    if (tty_fd < 0) {
        feedback = "Not allowed: this session has no terminal to ask for permission (run clil with --yes).";
        warn("  not allowed without a terminal (use --yes)");
        return 0;
    }
    LineEditor ed(tty_fd);
    std::string prompt = fmt("  %sallow %s?%s [Y]es / [n]o / [a]lways, or type instructions: ", ce(ansi::yellow),
                             tool.c_str(), ce(ansi::reset));
    std::string ans;
    LineEditor::Result r = ed.read(prompt, display_width(prompt), ans);
    if (r != LineEditor::Line) return -1;
    std::string a = lower(trim(ans));
    if (a.empty() || a == "y" || a == "yes" || a == "j" || a == "ja") return 1;
    if (a == "a" || a == "always" || a == "altijd") {
        always_allow.insert(tool);
        return 1;
    }
    if (a == "n" || a == "no" || a == "nee") return -1;
    feedback = "The user declined this tool call and said: " + trim(ans);
    return 0;
}

std::string App::exec_tool(const ToolCall& c, bool& stop) {
    int cols = term_cols();
    Json args;
    std::string perr;
    bool parsed = Json::parse(c.args, args, &perr) && args.is_obj();
    std::string title = parsed ? tool_title(c.name, args) : "(invalid arguments)";
    std::string head =
        fmt("%s●%s %s%s%s ", ce(ansi::green), ce(ansi::reset), ce(ansi::bold), c.name.c_str(), ce(ansi::reset));
    size_t room = (size_t)std::max(20, cols - (int)c.name.size() - 4);
    fprintf(stderr, "%s%s\n", head.c_str(), utf8_clip(title, room).c_str());
    if (!parsed)
        return "Error: arguments are not a valid JSON object (" + perr + "). Call the tool again with valid JSON.";
    if (!tool_known(c.name)) return "Error: unknown tool '" + c.name + "'; available: bash, read, search, write, edit.";

    if (tool_mutates(c.name)) {
        for (auto& l : tool_change_preview(c.name, args)) {
            const char* col = l[0] == '+' ? ansi::green : l[0] == '-' ? ansi::red : ansi::dim;
            fprintf(stderr, "  %s%s%s\n", ce(col), utf8_clip(l, room).c_str(), ce(ansi::reset));
        }
        std::string feedback;
        int v = approve(c.name, feedback);
        if (v < 0) {
            stop = true;
            note("  └ denied");
            return "The user denied this tool call. Stop and wait for new instructions.";
        }
        if (v == 0) return feedback;
    }
    ToolOutput o = tool_run(c.name, args, tool_cap(), cfg.timeout_s);
    if (c.name == "write" || c.name == "edit") {
        std::string path = args["path"].str();
        if (o.error)
            failed_changes.insert(path);
        else
            failed_changes.erase(path);
    }
    fprintf(stderr, "  %s└ %s%s\n", ce(o.error ? ansi::red : ansi::dim), o.summary.c_str(), ce(ansi::reset));
    if (!cfg.quiet)
        for (auto& l : o.preview)
            fprintf(stderr, "    %s%s%s\n", ce(ansi::dim), utf8_clip(l, room).c_str(), ce(ansi::reset));
    if (g_interrupted) stop = true;
    return o.text;
}

// ------------------------------------------------------------- the turn

int App::run_turn(const std::string& user_text) {
    g_interrupted = 0;
    if (cfg.model.empty()) {
        error("no model selected — see /models, then /model <name> or /load <name>");
        return 1;
    }
    if (!ensure_model_ready()) return 1;

    failed_changes.clear();
    tool_new_turn();
    fit_context(false, user_text.size());
    size_t turn_start = msgs.size();
    msgs.push_back(message("user", user_text));
    long long gen_tokens = 0;
    double gen_secs = 0;
    std::string last_sig;
    int repeats = 0, rc = 0, nudges = 0;

    for (int step = 0;; ++step) {
        if (step >= cfg.max_steps) {
            warn(fmt("stopped after %d tool rounds (raise with --max-steps)", cfg.max_steps));
            break;
        }
        if (step > 0) fit_context(true, 0);
        Spinner sp;
        sp.start(step ? "working" : "thinking");
        Printer pr(cfg.think, sp);
        ChatEvents ev;
        ev.content = [&](const std::string& s) { pr.content(s); };
        ev.reasoning = [&](const std::string& s) { pr.reasoning(s); };
        ev.tool_progress = [&](const ToolCall& c) {
            if (!sp.active()) sp.start("");
            sp.label(fmt("%s … %s", c.name.c_str(), human_bytes((double)c.args.size()).c_str()));
            sp.tick();
        };
        ev.tick = [&] { sp.tick(); };
        ev.queued = [&](int n) {
            sp.label(n ? fmt("waiting for %s", plural((size_t)n, "other clil").c_str()) : step ? "working" : "thinking");
            sp.tick();
        };
        ChatReply rep = lm.chat(build_body(cfg.tools), ev);
        sp.stop();
        pr.finish();

        std::string clean = strip_think(rep.content);
        if (rep.interrupted || !rep.error.empty()) {
            bool unanswered = msgs.size() == turn_start + 1;
            if (!clean.empty())
                msgs.push_back(message("assistant", clean + (rep.interrupted ? " [interrupted]" : "")));
            else if (unanswered)
                msgs.pop_back();  // nothing came back: forget the question
            else
                msgs.push_back(message("assistant", "[stopped]"));
            if (rep.interrupted) {
                g_interrupted = 0;
                note("interrupted");
                return 130;
            }
            error(rep.error);
            return 1;
        }
        if (rep.prompt_tokens >= 0) ctx_used = rep.prompt_tokens + std::max(0LL, rep.completion_tokens);
        gen_tokens += rep.completion_tokens >= 0 ? rep.completion_tokens : rep.deltas;
        gen_secs += rep.gen_secs;
        ++stats.replies;
        stats.prompt += std::max(0LL, rep.prompt_tokens);
        stats.completion += rep.completion_tokens >= 0 ? rep.completion_tokens : rep.deltas;
        stats.secs += rep.gen_secs;

        std::vector<ToolCall> calls = rep.calls;
        if (calls.empty() && cfg.tools) extract_text_tool_calls(clean, calls);
        Json am = message("assistant", clean);
        if (!calls.empty()) {
            Json tcs = Json::array();
            for (auto& c : calls) {
                if (c.id.empty()) c.id = new_call_id();
                Json parsed;
                bool ok = Json::parse(c.args, parsed) && parsed.is_obj();
                Json fn = Json::object();
                fn.set("name", c.name).set("arguments", ok ? c.args : std::string("{}"));
                Json tc = Json::object();
                tc.set("id", c.id).set("type", "function").set("function", fn);
                tcs.push(tc);
            }
            am.set("tool_calls", tcs);
        }
        msgs.push_back(am);
        if (rep.prompt_tokens >= 0) ctx_used_msgs = msgs.size();

        if (calls.empty() && !failed_changes.empty() && nudges < 2 && step + 1 < cfg.max_steps) {
            // It wants to stop although a file change failed: send it back once or twice.
            ++nudges;
            std::string paths;
            for (auto& p : failed_changes)
                paths += (paths.empty() ? "" : ", ") + p;
            note("  ↻ the change to " + paths + " failed; asking the model to fix it");
            msgs.push_back(message("user", "Your last change to " + paths +
                                               " FAILED, so the file is unchanged. Do not claim it worked. Call the "
                                               "tool again, copying old_string exactly from the file content shown "
                                               "in the error (or use write with the full new content)."));
            continue;
        }
        if (calls.empty()) {
            if (rep.finish == "length") warn("reply was cut off (token limit reached)");
            if (clean.empty() && !pr.answered()) warn("the model returned an empty reply");
            break;
        }
        bool stop = false;
        for (auto& c : calls) {
            std::string result = stop ? "Skipped: the user stopped this turn." : exec_tool(c, stop);
            Json tm = message("tool", result);
            tm.set("tool_call_id", c.id);
            msgs.push_back(tm);
        }
        if (stop) {
            rc = g_interrupted ? 130 : 0;
            g_interrupted = 0;
            break;
        }
        std::string sig;
        for (auto& c : calls)
            sig += c.name + c.args;
        repeats = sig == last_sig ? repeats + 1 : 0;
        last_sig = sig;
        if (repeats >= 2) {
            warn("the model keeps repeating the same tool call; stopping");
            break;
        }
    }
    // Small models sometimes claim success after a failed edit: say what really happened.
    for (auto& path : failed_changes)
        warn("⚠ " + path + " was not changed: the last edit/write of it failed");
    if ((interactive || isatty(2)) && !cfg.quiet && gen_tokens > 0) {
        std::string ctx = human_tokens(estimate_tokens());
        if (ctx_len > 0) ctx += "/" + human_ctx(ctx_len);
        std::string speed = gen_secs > 0.05 ? fmt(" · %.1f tok/s", (double)gen_tokens / gen_secs) : "";
        fprintf(stderr, "%s  %s tok%s · ctx %s%s\n", ce(ansi::gray), human_tokens(gen_tokens).c_str(), speed.c_str(),
                ctx.c_str(), ce(ansi::reset));
    }
    return rc;
}
