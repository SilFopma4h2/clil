// Slash commands, LM Studio model management, completion and @file mentions.
#include "app.hpp"
#include "mcp.hpp"
#include "term.hpp"
#include "tools.hpp"
#include "util.hpp"

#include <dirent.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

std::string find_lms() {
    std::string p = find_in_path("lms");
    if (!p.empty()) return p;
    for (const char* rel : {"/.lmstudio/bin/lms", "/.cache/lm-studio/bin/lms"}) {
        std::string c = home_dir() + rel;
        if (is_file(c)) return c;
    }
    return "";
}

// ------------------------------------------------------------- models

bool App::refresh_models(std::string& err) {
    return lm.list_models(models, err);
}

const ModelInfo* App::find_model(const std::string& name, std::string* err) const {
    for (auto& m : models)
        if (m.key == name) return &m;
    for (auto& m : models)
        for (auto& li : m.loaded)
            if (li.first == name) return &m;
    // unique case-insensitive substring: "/load coder" finds "qwen2.5-coder-7b-instruct"
    const ModelInfo* hit = nullptr;
    std::string want = lower(name);
    std::vector<std::string> many;
    for (auto& m : models)
        if (lower(m.key).find(want) != std::string::npos) {
            hit = &m;
            many.push_back(m.key);
        }
    if (many.size() == 1) return hit;
    if (err) {
        if (many.empty())
            *err = "no model matches '" + name + "' (see /models)";
        else {
            *err = "'" + name + "' matches several models:";
            for (auto& k : many)
                *err += "\n  " + k;
        }
    }
    return nullptr;
}

std::string App::resolve_model(const std::string& name, std::string* err) const {
    const ModelInfo* m = find_model(name, err);
    if (!m) return "";
    for (auto& li : m->loaded)
        if (li.first == name) return li.first;
    return m->loaded.empty() ? m->key : m->loaded[0].first;
}

void App::select_model(const std::string& id) {
    cfg.model = id;
    ctx_len = 0;
    if (const ModelInfo* m = find_model(id))
        for (auto& li : m->loaded)
            if (li.first == id || m->loaded.size() == 1) ctx_len = li.second;
}

void App::pick_default_model() {
    for (auto& m : models)
        if (m.type == "llm" && !m.loaded.empty()) return select_model(m.loaded[0].first);
    std::vector<const ModelInfo*> llms;
    for (auto& m : models)
        if (m.type == "llm") llms.push_back(&m);
    if (llms.size() == 1) select_model(llms[0]->key);  // the only one: it gets loaded on first use
}

bool App::load_model(const std::string& key, int ctx) {
    Spinner sp;
    sp.start("loading " + key);
    LoadResult lr;
    std::string err;
    int r = lm.load(key, ctx, lr, err, [&] { sp.tick(); });
    sp.stop();
    if (r == 1) {
        note(fmt("loaded %s in %.1fs · context %s", lr.instance.c_str(), lr.secs,
                 lr.ctx ? human_ctx(lr.ctx).c_str() : "default"));
        std::string e;
        refresh_models(e);
        select_model(lr.instance);
        if (lr.ctx) ctx_len = lr.ctx;
        return true;
    }
    if (r == 0) {  // LM Studio < 0.4 has no load endpoint: use its CLI
        std::string lms = find_lms();
        if (lms.empty()) {
            error("this LM Studio version cannot load models over HTTP and the `lms` CLI was not found");
            return false;
        }
        std::vector<std::string> argv = {lms, "load", key, "-y"};
        if (ctx > 0) argv.insert(argv.end(), {"--context-length", std::to_string(ctx)});
        if (run_attached(argv) != 0) return false;
        std::string e;
        refresh_models(e);
        select_model(key);
        return true;
    }
    if (g_interrupted) {
        g_interrupted = 0;
        note("stopped waiting (LM Studio may still finish loading)");
    } else
        error(err);
    return false;
}

bool App::ensure_model_ready() {
    if (lm.api == 2) return true;  // not LM Studio: nothing to manage
    std::string err;
    if (!refresh_models(err)) {
        error(err);
        if (err.find("cannot connect") != std::string::npos)
            note("is the LM Studio server running? start it in the app (Developer tab) or with /server start");
        return false;
    }
    const ModelInfo* m = nullptr;  // exact key or instance id only
    for (auto& x : models) {
        if (x.key == cfg.model) m = &x;
        for (auto& li : x.loaded)
            if (li.first == cfg.model) m = &x;
    }
    if (!m) return true;  // not a name we know: let the server decide
    if (!m->loaded.empty()) {
        select_model(cfg.model);
        if (!ctx_len) ctx_len = m->loaded[0].second;
        return true;
    }
    if (m->type == "embedding") {
        error(cfg.model + " is an embedding model, not a chat model");
        return false;
    }
    if (lm.api == 0) return true;  // older LM Studio loads on demand (JIT)
    return load_model(m->key, cfg.load_ctx);
}

// ------------------------------------------------------------- printing helpers

static void print_models(const std::vector<ModelInfo>& models, bool loaded_only, const std::string& current) {
    size_t w = 10;
    for (auto& m : models)
        w = std::max(w, m.key.size());
    w = std::min<size_t>(w, 48);
    int shown = 0;
    for (auto& m : models) {
        if (loaded_only && m.loaded.empty()) continue;
        ++shown;
        bool is_cur = m.key == current;
        for (auto& li : m.loaded)
            is_cur |= li.first == current;
        std::string info;
        if (!m.params.empty()) info += m.params + " ";
        if (!m.quant.empty()) info += m.quant + " ";
        if (!m.format.empty()) info += m.format + " ";
        if (m.size) info += human_bytes((double)m.size) + " ";
        if (m.max_ctx) info += "ctx " + human_ctx(m.max_ctx) + " ";
        if (m.tools) info += "tools ";
        if (m.vision) info += "vision ";
        if (m.type != "llm") info += m.type + " ";
        const char* dot = m.loaded.empty() ? "○" : "●";
        printf("%s%s%s %s%-*s%s  %s%s%s\n", m.loaded.empty() ? co(ansi::gray) : co(ansi::green), dot, co(ansi::reset),
               is_cur ? co(ansi::bold) : "", (int)w, utf8_clip(m.key, w).c_str(), co(ansi::reset), co(ansi::dim),
               trim(info).c_str(), co(ansi::reset));
        if (loaded_only || m.loaded.size() > 1 || (m.loaded.size() == 1 && m.loaded[0].first != m.key))
            for (auto& li : m.loaded)
                printf("    %s└ %s · context %s%s\n", co(ansi::dim), li.first.c_str(), human_ctx(li.second).c_str(),
                       co(ansi::reset));
    }
    if (!shown) note(loaded_only ? "no models loaded (use /load <model>)" : "no models downloaded (use /get <model>)");
}

// Single source for /help and the live command menu.
struct CmdInfo {
    const char* usage;
    const char* desc;
};
static const CmdInfo kCmds[] = {
    {"/models", "list downloaded models (● loaded)"},
    {"/ps", "list loaded models"},
    {"/model [name]", "show or switch the chat model"},
    {"/load <name> [ctx]", "load a model, optionally with a context length"},
    {"/unload [name|all]", "unload a model (default: current)"},
    {"/get <name|url>", "download a model from the LM Studio catalog / Hugging Face"},
    {"/server [start|stop|status]", "control the LM Studio server (via lms)"},
    {"/lms <args>", "run any lms command, e.g. /lms log stream"},
    {"/clear", "start a new conversation"},
    {"/compact", "summarize the conversation to free context"},
    {"/ctx", "show context usage"},
    {"/system [text|off]", "show or set extra system instructions"},
    {"/temp [x|off]", "show or set the temperature"},
    {"/mcp [connect|disconnect <name>]", "list or connect MCP servers from ~/.lmstudio/mcp.json"},
    {"/tools", "toggle tools (off = plain chat, smaller prompts)"},
    {"/yes", "toggle auto-approve for bash/write/edit"},
    {"/think", "toggle showing the model's reasoning"},
    {"/save [file]", "save the conversation as Markdown"},
    {"/help", "show this list"},
    {"/exit", "quit (also Ctrl-D)"},
};

static void print_help() {
    static const char* rows[][2] = {
        {"!<cmd>", "run a shell command yourself"},
        {"@path", "attach a file to your message"},
    };
    for (auto& c : kCmds)
        printf("  %s%-28s%s %s\n", co(ansi::cyan), c.usage, co(ansi::reset), c.desc);
    for (auto& r : rows)
        printf("  %s%-28s%s %s\n", co(ansi::cyan), r[0], co(ansi::reset), r[1]);
    printf("\n  %sEnter sends · Ctrl-J or a trailing \\ adds a line · Tab completes · Ctrl-C stops a reply%s\n",
           co(ansi::dim), co(ansi::reset));
}

static bool save_markdown(const std::vector<Json>& msgs, const std::string& path, std::string& err) {
    std::string md = "# clil conversation\n\n";
    for (size_t i = 1; i < msgs.size(); ++i) {
        const Json& m = msgs[i];
        std::string role = m["role"].str(), c = m["content"].str();
        if (role == "user")
            md += "## You\n\n" + c + "\n\n";
        else if (role == "assistant") {
            md += "## clil\n\n";
            if (!c.empty()) md += c + "\n\n";
            for (auto& tc : m["tool_calls"].items())
                md += "`" + tc["function"]["name"].str() + "` `" + utf8_clip(tc["function"]["arguments"].str(), 300) +
                      "`\n\n";
        } else if (role == "tool")
            md += "```\n" + c + "\n```\n\n";
    }
    return write_file(path, md, err);
}

static void download(App& app, const std::string& model) {
    Json res;
    std::string err;
    int r = app.lm.download(model, res, err);
    if (r == 0) {
        std::string lms = find_lms();
        if (lms.empty()) return error("this LM Studio version cannot download over HTTP; use the app or `lms get`");
        run_attached({lms, "get", model});
        return;
    }
    if (r < 0) return error(err);
    std::string status = res["status"].str();
    if (status == "already_downloaded") return note(model + " is already downloaded");
    std::string job = res["job_id"].str();
    if (job.empty()) return note("download status: " + status);
    note("downloading " + model + " (Ctrl-C stops watching; the download continues in LM Studio)");
    g_interrupted = 0;
    double total = res["total_size_bytes"].num();
    for (;;) {
        Json st;
        if (!app.lm.download_status(job, st, err)) return error(err);
        status = st["status"].str();
        double got = st["downloaded_bytes"].num(), tot = st["total_size_bytes"].num(total);
        if (isatty(2) && tot > 0) {
            int pct = (int)(got * 100 / tot), bar = pct / 4;
            fprintf(stderr, "\r\x1b[K  [%s%s%s%s] %3d%% %s / %s %s", ce(ansi::green),
                    std::string((size_t)bar, '#').c_str(), std::string((size_t)(25 - bar), ' ').c_str(),
                    ce(ansi::reset), pct, human_bytes(got).c_str(), human_bytes(tot).c_str(),
                    st["bytes_per_second"].is_num() ? (human_bytes(st["bytes_per_second"].num()) + "/s").c_str() : "");
            fflush(stderr);
        }
        if (status != "downloading" && status != "paused") break;
        for (int i = 0; i < 10 && !g_interrupted; ++i)
            usleep(50000);
        if (g_interrupted) {
            g_interrupted = 0;
            if (isatty(2)) fputc('\n', stderr);
            return note("stopped watching; job " + job + " keeps running in LM Studio");
        }
    }
    if (isatty(2)) fputc('\n', stderr);
    if (status == "completed")
        note("downloaded " + model + " — load it with /load " + model);
    else
        error("download " + status);
}

// ------------------------------------------------------------- dispatch

bool App::command(const std::string& line) {
    std::vector<std::string> a = split_args(line);
    if (a.empty()) return true;
    std::string cmd = lower(a[0]);
    size_t sp = line.find_first_of(" \t\n");
    std::string rest = sp == std::string::npos ? "" : trim(line.substr(sp));
    std::string err;

    if (cmd == "/exit" || cmd == "/quit" || cmd == "/q") return false;
    if (cmd == "/help" || cmd == "/?" || cmd == "/h") {
        print_help();
    } else if (cmd == "/models" || cmd == "/ls" || cmd == "/ps") {
        if (!refresh_models(err)) return error(err), true;
        print_models(models, cmd == "/ps", cfg.model);
    } else if (cmd == "/model") {
        if (a.size() < 2) {
            if (cfg.model.empty())
                note("no model selected");
            else
                printf("%s%s\n", cfg.model.c_str(), ctx_len ? (" · context " + human_ctx(ctx_len)).c_str() : "");
            return true;
        }
        refresh_models(err);
        std::string id = models.empty() ? a[1] : resolve_model(a[1], &err);
        if (id.empty()) return error(err), true;
        select_model(id);
        const ModelInfo* m = find_model(id);
        note("model: " + id + (m && m->loaded.empty() ? " (not loaded yet — loads with your next message)" : ""));
    } else if (cmd == "/load") {
        if (a.size() < 2) return error("usage: /load <model> [context-length]"), true;
        refresh_models(err);
        const ModelInfo* m = find_model(a[1], &err);
        if (!m && !models.empty()) return error(err), true;
        int ctx = a.size() > 2 ? atoi(a[2].c_str()) : cfg.load_ctx;
        g_interrupted = 0;
        load_model(m ? m->key : a[1], ctx);
    } else if (cmd == "/unload") {
        refresh_models(err);
        std::vector<std::string> ids;
        std::string target = a.size() > 1 ? a[1] : cfg.model;
        if (target.empty()) return error("usage: /unload <model|all>"), true;
        if (target == "all") {
            for (auto& m : models)
                for (auto& li : m.loaded)
                    ids.push_back(li.first);
        } else if (const ModelInfo* m = find_model(target, &err)) {
            bool exact = false;
            for (auto& li : m->loaded)
                exact |= li.first == target;
            for (auto& li : m->loaded)
                if (!exact || li.first == target) ids.push_back(li.first);
        } else
            return error(err), true;
        if (ids.empty()) return note("nothing loaded to unload"), true;
        for (auto& id : ids) {
            int r = lm.unload(id, err);
            if (r == 0) {
                std::string lms = find_lms();
                if (lms.empty()) return error("cannot unload over HTTP with this LM Studio version; use the app"), true;
                run_attached({lms, "unload", id});
            } else if (r < 0)
                error(err);
            else
                note("unloaded " + id);
        }
        refresh_models(err);
        if (const ModelInfo* m = find_model(cfg.model))
            if (m->loaded.empty()) ctx_len = 0;
    } else if (cmd == "/get" || cmd == "/download") {
        if (a.size() < 2) return error("usage: /get <model>, e.g. /get qwen/qwen3-4b"), true;
        download(*this, a[1]);
    } else if (cmd == "/server" || cmd == "/lms") {
        std::string lms = find_lms();
        if (lms.empty())
            return error("`lms` not found; open LM Studio once, or see https://lmstudio.ai/docs/cli"), true;
        std::vector<std::string> argv = {lms};
        if (cmd == "/server") argv.push_back("server");
        argv.insert(argv.end(), a.begin() + 1, a.end());
        if (cmd == "/server" && a.size() == 1) argv.push_back("status");
        run_attached(argv);
    } else if (cmd == "/clear" || cmd == "/new" || cmd == "/reset") {
        reset_conversation();
        note("new conversation");
    } else if (cmd == "/compact") {
        compact(false);
    } else if (cmd == "/ctx" || cmd == "/context") {
        size_t user = 0, tool = 0;
        for (auto& m : msgs)
            user += m["role"].str() == "user", tool += m["role"].str() == "tool";
        printf("~%s tokens%s · %zu messages (%zu from you, %zu tool results)\n",
               human_tokens(estimate_tokens()).c_str(), ctx_len ? (" of " + human_ctx(ctx_len)).c_str() : "",
               msgs.size(), user, tool);
    } else if (cmd == "/system") {
        if (rest.empty()) {
            printf("%s\n", msgs.empty() ? system_prompt().c_str() : msgs[0]["content"].str().c_str());
            return true;
        }
        cfg.system_extra = lower(rest) == "off" ? "" : rest;
        if (!msgs.empty()) msgs[0].set("content", system_prompt());
        note(cfg.system_extra.empty() ? "extra system instructions removed" : "system instructions updated");
    } else if (cmd == "/temp" || cmd == "/temperature") {
        if (a.size() > 1) cfg.temperature = lower(a[1]) == "off" ? -1 : atof(a[1].c_str());
        note(cfg.temperature < 0 ? "temperature: model default" : fmt("temperature: %.2f", cfg.temperature));
    } else if (cmd == "/mcp") {
        std::string sub = a.size() > 1 ? lower(a[1]) : "";
        std::string arg = a.size() > 2 ? a[2] : "";
        if (sub == "connect" || sub == "disconnect") {
            std::vector<std::string> names;
            for (auto& m : mcp_list())
                if (arg == "all" ? m.supported : m.name == arg) names.push_back(m.name);
            if (arg.empty() || (names.empty() && arg != "all")) names.push_back(arg);
            for (auto& n : names) {
                if (sub == "disconnect") {
                    if (mcp_disconnect(n)) note("disconnected " + n);
                    else warn(n + " is not connected");
                    continue;
                }
                note("connecting " + n + " …");
                if (!mcp_connect(n, err)) { warn(n + ": " + err); continue; }
                for (auto& m : mcp_list())
                    if (m.name == n) note(fmt("connected %s: %s", n.c_str(), plural(m.tools, "tool").c_str()));
            }
            if (!cfg.tools) warn("tools are off (/tools)");
        } else {
            auto list = mcp_list();
            if (list.empty()) note("no servers in ~/.lmstudio/mcp.json");
            for (auto& m : list)
                fprintf(stderr, "  %s %s  %s\n", m.connected ? "●" : "○", m.name.c_str(),
                        !m.supported ? "(remote url servers are not supported)"
                        : m.connected ? plural(m.tools, "tool").c_str() : "");
        }
    } else if (cmd == "/tools") {
        cfg.tools = !cfg.tools;
        if (!msgs.empty()) msgs[0].set("content", system_prompt());
        note(cfg.tools ? "tools on" : "tools off (plain chat)");
    } else if (cmd == "/yes" || cmd == "/yolo") {
        cfg.yes = !cfg.yes;
        note(cfg.yes ? "auto-approve on: bash/write/edit run without asking" : "auto-approve off");
    } else if (cmd == "/think") {
        cfg.think = !cfg.think;
        note(cfg.think ? "showing reasoning" : "hiding reasoning");
    } else if (cmd == "/save") {
        std::string path = a.size() > 1 ? expand_home(a[1]) : "";
        if (path.empty()) {
            char buf[64];
            time_t t = time(nullptr);
            strftime(buf, sizeof buf, "clil-%Y%m%d-%H%M%S.md", localtime(&t));
            path = buf;
        }
        if (save_markdown(msgs, path, err))
            note("saved " + path);
        else
            error(err);
    } else {
        error("unknown command " + a[0] + " (see /help)");
    }
    return true;
}

// ------------------------------------------------------------- completion & mentions

static void complete_path(const std::string& word, std::vector<std::string>& out) {
    std::string w = expand_home(word);
    size_t slash = w.find_last_of('/');
    std::string dir = slash == std::string::npos ? "." : w.substr(0, slash + 1);
    std::string base = slash == std::string::npos ? w : w.substr(slash + 1);
    std::string shown_dir = slash == std::string::npos ? "" : word.substr(0, word.find_last_of('/') + 1);
    DIR* d = opendir(dir.empty() ? "/" : dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == ".." || n.compare(0, base.size(), base) != 0) continue;
        if (n[0] == '.' && base.empty()) continue;
        out.push_back(shown_dir + n + (is_dir(dir + n) ? "/" : ""));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
}

void App::complete(const std::string& buf, size_t& start, std::vector<std::string>& out) {
    size_t sp = buf.find_last_of(" \t\n");
    start = sp == std::string::npos ? 0 : sp + 1;
    std::string word = buf.substr(start);
    if (start == 0 && !word.empty() && word[0] == '/') {
        std::vector<std::pair<std::string, std::string>> items;
        menu(word, items);
        for (auto& i : items)
            out.push_back(i.first);
        for (const char* c : {"/quit", "/download"})  // aliases: Tab only, not in the menu
            if (starts_with(c, word.c_str())) out.push_back(c);
        return;
    }
    if (!word.empty() && word[0] == '@') {
        complete_path(word.substr(1), out);
        for (auto& o : out)
            o = "@" + o;
        return;
    }
    std::string first = buf.substr(0, buf.find(' '));
    if (first == "/model" || first == "/load" || first == "/unload") {
        if (models.empty()) {
            std::string err;
            refresh_models(err);
        }
        std::string w = lower(word);
        for (auto& m : models) {
            if (m.type == "embedding" && first != "/unload") continue;
            if (first == "/unload" && m.loaded.empty()) continue;
            if (lower(m.key).find(w) != std::string::npos) out.push_back(m.key);
        }
        if (first == "/unload" && starts_with("all", w.c_str())) out.push_back("all");
        return;
    }
    if (first == "/mcp") {
        for (auto& m : mcp_list())
            if (starts_with(m.name, word.c_str())) out.push_back(m.name);
        for (const char* c : {"connect", "disconnect"})
            if (starts_with(c, word.c_str())) out.push_back(c);
        return;
    }
    if (first == "/save") complete_path(word, out);
}

void App::menu(const std::string& word, std::vector<std::pair<std::string, std::string>>& out) const {
    for (auto& c : kCmds) {
        std::string name = std::string(c.usage).substr(0, strcspn(c.usage, " "));
        if (starts_with(name, word.c_str())) out.push_back({name, c.desc});
    }
}

std::string App::expand_mentions(const std::string& text) const {
    std::string files;
    size_t cap = tool_cap();
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '@' || (i > 0 && !isspace((unsigned char)text[i - 1]))) continue;
        size_t e = i + 1;
        while (e < text.size() && !isspace((unsigned char)text[e]))
            ++e;
        std::string path = text.substr(i + 1, e - i - 1);
        while (!path.empty() && strchr(",.;:!?)'\"", path.back()))
            path.pop_back();
        if (path.empty() || (!is_file(expand_home(path)) && !is_dir(expand_home(path)))) continue;
        ToolOutput o = run_read(path, 0, 0, cap);
        files += "\n\n<file path=\"" + path + "\">\n" + o.text + "\n</file>";
        note("  attached " + path + " (" + o.summary + ")");
    }
    return text + files;
}
