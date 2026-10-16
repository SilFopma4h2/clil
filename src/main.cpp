// clil — a small, fast terminal coding agent for LM Studio.
#include "app.hpp"
#include "term.hpp"
#include "util.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void on_sigint(int) {
    g_interrupted = 1;
}

static void usage() {
    printf("clil " CLIL_VERSION
           " — terminal coding agent for LM Studio\n\n"
           "usage: clil [options]            interactive session\n"
           "       clil [options] PROMPT...  one-shot (stdin is appended when piped)\n"
           "       clil /COMMAND [args]      run one command, e.g. clil /models\n\n"
           "options:\n"
           "  -m, --model NAME     model to use (env LMSTUDIO_MODEL; default: the loaded model)\n"
           "  -H, --host HOST:PORT LM Studio server (env LMSTUDIO_HOST; default 127.0.0.1:1234)\n"
           "  -c, --ctx N          context length when clil loads a model\n"
           "  -s, --system TEXT    extra system instructions\n"
           "  -t, --temp X         temperature\n"
           "  -n, --max-tokens N   cap each reply\n"
           "  -y, --yes            run bash/write/edit without asking\n"
           "      --no-tools       plain chat, no tools (smallest prompts)\n"
           "      --no-think       hide the model's reasoning\n"
           "      --timeout SEC    limit per shell command (default 180)\n"
           "      --max-steps N    tool rounds per message (default 40)\n"
           "  -q, --quiet          no status line or tool output previews\n"
           "      --no-color       disable colours (also NO_COLOR)\n"
           "  -V, --version\n"
           "  -h, --help\n\n"
           "env: LM_API_TOKEN (if LM Studio requires authentication)\n"
           "In a session type /help for commands.\n");
}

static bool is_command(const std::string& line) {
    if (line.size() < 2 || line[0] != '/') return false;
    size_t sp = line.find_first_of(" \t\n");
    std::string first = line.substr(0, sp);
    return first.find('/', 1) == std::string::npos;  // "/etc/hosts …" is a message
}

static std::string read_all(int fd) {
    std::string out;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0) out.append(buf, (size_t)n);
    return out;
}

// The logo, drawn with half blocks on two terminal rows (same grid as assets/logo-*.svg).
static const char* kLogo[2][2] = {{"▀▄", "  ▄▄▄ █ ▀ █"}, {"▄▀", "  █▄▄ █ █ █"}};

static void banner(App& app, bool reachable, const std::string& err) {
    std::string line1 = fmt("%sclil%s%s %s", ce(ansi::bold), ce(ansi::reset), ce(ansi::gray), CLIL_VERSION);
    std::string line2;
    if (!reachable) {
        line1 += std::string(ce(ansi::reset)) + " · " + ce(ansi::yellow) + "LM Studio not reachable" + ce(ansi::reset);
        line2 = std::string(ce(ansi::gray)) + err + ce(ansi::reset);
    } else {
        line1 += " · LM Studio " + app.lm.ep.str() + ce(ansi::reset);
        if (app.cfg.model.empty())
            line2 = std::string(ce(ansi::yellow)) + "no model loaded" + ce(ansi::reset);
        else {
            line2 = std::string(ce(ansi::cyan)) + app.cfg.model + ce(ansi::reset);
            if (app.ctx_len) line2 += std::string(ce(ansi::gray)) + " (ctx " + human_ctx(app.ctx_len) + ")";
        }
        line2 += std::string(ce(ansi::gray)) + (app.cfg.tools ? " · tools" : " · chat only") +
                 (app.cfg.yes ? " · auto-approve" : "") + " · /help" + ce(ansi::reset);
    }
    const char* accent = g_color_err ? "\x1b[38;5;208m" : "";
    if (term_cols() >= 60) {
        fprintf(stderr, "\n%s%s%s%s   %s\n", accent, kLogo[0][0], ce(ansi::reset), kLogo[0][1], line1.c_str());
        fprintf(stderr, "%s%s%s%s   %s\n\n", accent, kLogo[1][0], ce(ansi::reset), kLogo[1][1], line2.c_str());
    } else {
        fprintf(stderr, "%s\n%s\n", line1.c_str(), line2.c_str());
    }
    if (!reachable)
        note("start the server in LM Studio (Developer tab) or with /server start, then /models");
    else if (app.cfg.model.empty())
        note("pick one: /models, then /load <name> (Tab completes)");
}

int main(int argc, char** argv) {
    struct sigaction sa = {};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);  // no SA_RESTART: blocking calls return EINTR
    signal(SIGPIPE, SIG_IGN);

    App app;
    std::string host, err;
    for (const char* k : {"LMSTUDIO_HOST", "LM_STUDIO_HOST"})
        if (getenv(k) && *getenv(k)) host = getenv(k);
    if (const char* t = getenv("LM_API_TOKEN")) app.lm.ep.token = t;
    if (const char* m = getenv("LMSTUDIO_MODEL")) app.cfg.model = m;
    bool no_color = getenv("NO_COLOR") && *getenv("NO_COLOR");

    std::vector<std::string> words;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "clil: %s needs a value\n", s.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (s == "--") {
            for (++i; i < argc; ++i)
                words.push_back(argv[i]);
        } else if (s == "-m" || s == "--model")
            app.cfg.model = val();
        else if (s == "-H" || s == "--host")
            host = val();
        else if (s == "-c" || s == "--ctx")
            app.cfg.load_ctx = atoi(val().c_str());
        else if (s == "-s" || s == "--system")
            app.cfg.system_extra = val();
        else if (s == "-t" || s == "--temp")
            app.cfg.temperature = atof(val().c_str());
        else if (s == "-n" || s == "--max-tokens")
            app.cfg.max_tokens = atoi(val().c_str());
        else if (s == "-y" || s == "--yes")
            app.cfg.yes = true;
        else if (s == "--no-tools")
            app.cfg.tools = false;
        else if (s == "--no-think")
            app.cfg.think = false;
        else if (s == "--timeout")
            app.cfg.timeout_s = atoi(val().c_str());
        else if (s == "--max-steps")
            app.cfg.max_steps = std::max(1, atoi(val().c_str()));
        else if (s == "-q" || s == "--quiet")
            app.cfg.quiet = true;
        else if (s == "--no-color")
            no_color = true;
        else if (s == "-h" || s == "--help")
            return usage(), 0;
        else if (s == "-V" || s == "--version")
            return puts("clil " CLIL_VERSION), 0;
        else if (s.size() > 1 && s[0] == '-') {
            fprintf(stderr, "clil: unknown option %s (see --help)\n", s.c_str());
            return 2;
        } else
            words.push_back(s);
    }
    g_color_out = !no_color && isatty(1);
    g_color_err = !no_color && isatty(2);
    if (!host.empty() && !parse_endpoint(host, app.lm.ep, err)) {
        fprintf(stderr, "clil: %s\n", err.c_str());
        return 2;
    }
    app.tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (app.tty_fd < 0 && isatty(0)) app.tty_fd = 0;

    std::string prompt;
    for (auto& w : words)
        prompt += (prompt.empty() ? "" : " ") + w;
    // Read stdin when something is piped in (or no prompt was given); an idle
    // socket or device as stdin (scripts, IDEs) must not make us wait forever.
    struct stat st;
    bool piped = !isatty(0) && fstat(0, &st) == 0 &&
                 (S_ISFIFO(st.st_mode) || S_ISREG(st.st_mode) || (prompt.empty() && !S_ISCHR(st.st_mode)));
    bool reachable = app.refresh_models(err);
    if (reachable && app.cfg.model.empty())
        app.pick_default_model();
    else if (reachable) {
        std::string id = app.resolve_model(app.cfg.model, &err);
        if (id.empty()) warn(err);
        app.select_model(id.empty() ? app.cfg.model : id);
    }

    // ---- one-shot: a prompt on the command line and/or piped input
    if (!prompt.empty() || piped || !isatty(0)) {
        if (is_command(prompt)) return app.command(prompt), g_errors ? 1 : 0;
        std::string input = piped ? read_all(0) : "";
        if (!reachable) {
            fprintf(stderr, "clil: %s\n", err.c_str());
            return 1;
        }
        if (!input.empty()) {
            size_t cap = app.ctx_len > 0 ? (size_t)app.ctx_len * 2 : 32768;
            input = head_tail(utf8_clean(input), cap);
            prompt = prompt.empty() ? input : prompt + "\n\n```\n" + input + (input.back() == '\n' ? "" : "\n") + "```";
        }
        if (trim(prompt).empty()) return usage(), 2;
        app.reset_conversation();
        return app.run_turn(app.expand_mentions(prompt));
    }

    // ---- interactive session
    app.interactive = true;
    banner(app, reachable, err);
    LineEditor ed(app.tty_fd >= 0 ? app.tty_fd : 0);
    ed.history_load(home_dir() + "/.clil_history");
    ed.completer = [&](const std::string& buf, size_t& start, std::vector<std::string>& out) {
        app.complete(buf, start, out);
    };
    app.reset_conversation();
    std::string prompt_str = fmt("%s›%s ", ce(ansi::cyan), ce(ansi::reset));
    int cancels = 0;
    for (;;) {
        std::string line;
        LineEditor::Result r = ed.read(prompt_str, 2, line);
        if (r == LineEditor::Eof) break;
        if (r == LineEditor::Cancel) {
            if (++cancels >= 2) break;
            note("(Ctrl-C again or Ctrl-D to quit)");
            continue;
        }
        cancels = 0;
        while (!line.empty() && (line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (trim(line).empty()) continue;
        ed.history_add(line);
        std::string t = trim(line);
        if (is_command(t)) {
            if (!app.command(t)) break;
        } else if (t[0] == '!') {
            run_attached({"/bin/sh", "-c", t.substr(1)});
        } else {
            app.run_turn(app.expand_mentions(line));
        }
        fputc('\n', stderr);
    }
    return 0;
}
