#include "tools.hpp"
#include "util.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

static const char* kSchemas = R"([
{"type":"function","function":{"name":"bash","description":"Run a shell command in the working directory (fresh non-interactive sh each call). Returns output and exit code.","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}}},
{"type":"function","function":{"name":"read","description":"Read a text file, or list a directory.","parameters":{"type":"object","properties":{"path":{"type":"string"},"offset":{"type":"integer","description":"first line, 1-based"},"limit":{"type":"integer","description":"max lines"}},"required":["path"]}}},
{"type":"function","function":{"name":"write","description":"Create or overwrite a file.","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}}},
{"type":"function","function":{"name":"edit","description":"Replace one exact, unique occurrence of old_string in a file with new_string.","parameters":{"type":"object","properties":{"path":{"type":"string"},"old_string":{"type":"string"},"new_string":{"type":"string"}},"required":["path","old_string","new_string"]}}}
])";

Json tool_schemas() {
    Json j;
    Json::parse(kSchemas, j);
    return j;
}

bool tool_mutates(const std::string& n) {
    return n == "bash" || n == "write" || n == "edit";
}
bool tool_known(const std::string& n) {
    return n == "bash" || n == "read" || n == "write" || n == "edit";
}

// Small models drift on argument names; accept the common aliases.
static std::string arg_str(const Json& a, std::initializer_list<const char*> keys, bool* found = nullptr) {
    for (const char* k : keys) {
        const Json& v = a[k];
        if (v.is_str()) {
            if (found) *found = true;
            return v.str();
        }
        if (v.is_num() || v.is_bool()) {
            if (found) *found = true;
            return v.dump();
        }
    }
    if (found) *found = false;
    return "";
}

static long arg_long(const Json& a, const char* key, long def) {
    const Json& v = a[key];
    if (v.is_num()) return (long)v.integer();
    if (v.is_str() && !v.str().empty()) return atol(v.str().c_str());
    return def;
}

static ToolOutput fail(const std::string& msg) {
    ToolOutput o;
    o.text = "Error: " + msg;
    o.summary = msg;
    o.error = true;
    return o;
}

static std::vector<std::string> marked(const std::string& text, const char* mark, size_t max) {
    std::vector<std::string> out;
    auto lines = split_lines(text);
    for (size_t i = 0; i < lines.size() && i < max; ++i)
        out.push_back(mark + lines[i]);
    if (lines.size() > max) out.push_back(fmt("%s… +%zu lines", mark, lines.size() - max));
    return out;
}

std::string tool_title(const std::string& name, const Json& a) {
    if (name == "bash") {
        std::string c = arg_str(a, {"command", "cmd"});
        size_t nl = c.find('\n');
        return nl == std::string::npos ? c : c.substr(0, nl) + " …";
    }
    std::string path = arg_str(a, {"path", "file_path", "file"});
    if (name == "read") {
        long off = arg_long(a, "offset", 0), lim = arg_long(a, "limit", 0);
        if (off > 0 || lim > 0) path += fmt(" [%ld+%ld]", off > 0 ? off : 1, lim);
        return path;
    }
    if (name == "write") return path + " (" + plural(count_lines(arg_str(a, {"content", "text"})), "line") + ")";
    return path;
}

std::vector<std::string> tool_change_preview(const std::string& name, const Json& a) {
    if (name == "write") return marked(arg_str(a, {"content", "text"}), "+ ", 8);
    if (name == "edit") {
        auto out = marked(arg_str(a, {"old_string", "old"}), "- ", 8);
        auto add = marked(arg_str(a, {"new_string", "new"}), "+ ", 8);
        out.insert(out.end(), add.begin(), add.end());
        return out;
    }
    if (name == "bash") {
        auto lines = split_lines(arg_str(a, {"command", "cmd"}));
        if (lines.size() > 1) return marked(arg_str(a, {"command", "cmd"}), "  ", 12);
    }
    return {};
}

// ------------------------------------------------------------------ read

ToolOutput run_read(const std::string& path, long offset, long limit, size_t cap) {
    std::string p = expand_home(path);
    if (p.empty()) return fail("missing 'path'");
    ToolOutput o;
    if (is_dir(p)) {
        DIR* d = opendir(p.c_str());
        if (!d) return fail(p + ": " + strerror(errno));
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            names.push_back(is_dir(p + "/" + n) ? n + "/" : n);
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            if (o.text.size() + n.size() + 1 > cap) {
                o.text += fmt("[... %zu entries total]\n", names.size());
                break;
            }
            o.text += n + "\n";
        }
        if (names.empty()) o.text = "(empty directory)";
        o.summary = plural(names.size(), "entry", "entries");
        return o;
    }
    std::string data, err;
    if (!read_file(p, data, err)) return fail(err);
    if (memchr(data.data(), 0, std::min<size_t>(data.size(), 8192))) {
        o.text = fmt("(binary file, %zu bytes, not shown)", data.size());
        o.summary = "binary file";
        return o;
    }
    auto lines = split_lines(data);
    long total = (long)lines.size();
    long first = offset > 0 ? offset : 1;
    long max = limit > 0 ? limit : total;
    long i = first - 1;
    for (; i < total && i - (first - 1) < max; ++i) {
        std::string l = lines[(size_t)i];
        if (l.size() > 500) l = utf8_clip(l, 500);
        if (o.text.size() + l.size() + 1 > cap) break;
        o.text += l + "\n";
    }
    long shown = i - (first - 1);
    if (total == 0)
        o.text = "(empty file)";
    else if (first > total)
        o.text = fmt("(file has only %ld lines)", total);
    else if (first > 1 || i < total)
        o.text += fmt("[lines %ld-%ld of %ld%s]", first, i, total,
                      i < total ? fmt("; continue with offset=%ld", i + 1).c_str() : "");
    o.text = utf8_clean(o.text);
    o.summary = (first > 1 || i < total) ? fmt("lines %ld-%ld of %ld", first, i, total) : plural((size_t)shown, "line");
    return o;
}

// ------------------------------------------------------------------ edit

ToolOutput run_edit(const std::string& path, const std::string& old_s, const std::string& new_s) {
    std::string p = expand_home(path);
    if (p.empty()) return fail("missing 'path'");
    if (old_s.empty()) return fail("old_string is empty; use write to create a file");
    std::string data, err;
    if (!read_file(p, data, err)) return fail(err);

    size_t at = std::string::npos, n = 0;
    for (size_t f = data.find(old_s); f != std::string::npos; f = data.find(old_s, f + 1))
        if (++n == 1) at = f;
    if (n > 1) return fail(fmt("old_string occurs %zu times in %s; include more surrounding lines", n, path.c_str()));

    size_t len = old_s.size();
    std::string repl = new_s;
    if (n == 0) {
        // Fallback: same lines ignoring indentation / trailing spaces.
        auto want = split_lines(old_s);
        while (!want.empty() && trim(want.back()).empty())
            want.pop_back();
        while (!want.empty() && trim(want.front()).empty())
            want.erase(want.begin());
        std::vector<size_t> starts;  // byte offset of every line
        for (size_t s = 0; s < data.size();) {
            starts.push_back(s);
            size_t e = data.find('\n', s);
            if (e == std::string::npos) break;
            s = e + 1;
        }
        size_t k = want.size(), hits = 0, hit = 0;
        for (size_t i = 0; k && i + k <= starts.size(); ++i) {
            bool ok = true;
            for (size_t j = 0; ok && j < k; ++j) {
                size_t s = starts[i + j];
                size_t e = data.find('\n', s);
                if (e == std::string::npos) e = data.size();
                ok = trim(data.substr(s, e - s)) == trim(want[j]);
            }
            if (ok) hits++, hit = i;
        }
        if (hits != 1)
            return fail(hits ? fmt("old_string matches %zu places in %s; include more lines", hits, path.c_str())
                             : "old_string not found in " + path + "; read the file and copy the text exactly");
        at = starts[hit];
        size_t e = data.find('\n', starts[hit + k - 1]);
        len = (e == std::string::npos ? data.size() : e) - at;
        while (!repl.empty() && repl.back() == '\n')
            repl.pop_back();
        while (!repl.empty() && repl.front() == '\n')
            repl.erase(0, 1);
    }
    data.replace(at, len, repl);
    if (!write_file(p, data, err)) return fail(err);
    size_t line = 1 + (size_t)std::count(data.begin(), data.begin() + (long)at, '\n');
    ToolOutput o;
    o.text = fmt("Edited %s at line %zu (-%zu +%zu lines)", path.c_str(), line, count_lines(old_s), count_lines(new_s));
    o.summary = fmt("line %zu: -%zu +%zu", line, count_lines(old_s), count_lines(new_s));
    return o;
}

// ------------------------------------------------------------------ bash

ToolOutput run_bash(const std::string& cmd, size_t cap, int timeout_s) {
    if (trim(cmd).empty()) return fail("missing 'command'");
    int pfd[2];
    if (pipe(pfd) < 0) return fail(std::string("pipe: ") + strerror(errno));
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return fail(std::string("fork: ") + strerror(errno));
    if (pid == 0) {
        setpgid(0, 0);  // own group: ^C and timeouts kill the whole pipeline
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        setenv("PAGER", "cat", 1);
        setenv("GIT_PAGER", "cat", 1);
        setenv("GIT_TERMINAL_PROMPT", "0", 1);
        setenv("NO_COLOR", "1", 1);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    setpgid(pid, pid);
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK);

    std::string out;
    size_t dropped = 0;
    double t0 = now_secs(), exited_at = 0, term_at = 0;
    int status = 0;
    bool done = false, timed_out = false, interrupted = false, eof = false;
    char buf[8192];
    for (;;) {
        if (eof) {
            usleep(50000);
        } else {
            pollfd p{pfd[0], POLLIN, 0};
            if (poll(&p, 1, 100) > 0) {
                ssize_t n = read(pfd[0], buf, sizeof buf);
                if (n > 0) {
                    if (out.size() < (4u << 20))
                        out.append(buf, (size_t)n);
                    else
                        dropped += (size_t)n;
                } else if (n == 0)
                    eof = true;
            }
        }
        if (!done && waitpid(pid, &status, WNOHANG) == pid) done = true, exited_at = now_secs();
        double t = now_secs();
        // Background jobs may keep the pipe open; stop shortly after the shell exits.
        if (done && (eof || t - exited_at > 0.3)) break;
        if (!done && term_at == 0 && (g_interrupted || (timeout_s > 0 && t - t0 > timeout_s))) {
            interrupted = g_interrupted != 0;
            timed_out = !interrupted;
            kill(-pid, SIGTERM);
            term_at = t;
        }
        if (!done && term_at > 0 && t - term_at > 2) kill(-pid, SIGKILL);
    }
    close(pfd[0]);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);

    std::string text = utf8_clean(clean_terminal_output(out));
    while (!text.empty() && text.back() == '\n')
        text.pop_back();
    size_t nlines = count_lines(text);
    ToolOutput o;
    auto lines = split_lines(text);
    size_t show = std::min<size_t>(lines.size(), 6);
    size_t from = code != 0 ? lines.size() - show : 0;  // failures: the tail matters
    for (size_t i = from; i < from + show; ++i)
        o.preview.push_back(utf8_clip(lines[i], 300));
    if (lines.size() > show) o.preview.push_back(fmt("… %zu more lines", lines.size() - show));

    o.text = head_tail(text, cap);
    if (dropped) o.text += fmt("\n[%zu more bytes not captured]", dropped);
    if (timed_out) o.text += fmt("\n[killed: timed out after %ds]", timeout_s);
    if (interrupted) o.text += "\n[interrupted by user]";
    o.text += o.text.empty() ? fmt("(no output) [exit %d]", code) : fmt("\n[exit %d]", code);
    o.summary =
        timed_out ? fmt("timed out after %ds", timeout_s) : fmt("exit %d · %s", code, plural(nlines, "line").c_str());
    o.error = code != 0;
    return o;
}

// ------------------------------------------------------------------ dispatch

ToolOutput tool_run(const std::string& name, const Json& a, size_t cap, int timeout_s) {
    if (!a.is_obj()) return fail("arguments must be a JSON object");
    if (name == "bash") return run_bash(arg_str(a, {"command", "cmd"}), cap, timeout_s);
    std::string path = arg_str(a, {"path", "file_path", "file"});
    if (name == "read") return run_read(path, arg_long(a, "offset", 0), arg_long(a, "limit", 0), cap);
    if (name == "write") {
        bool has = false;
        std::string content = arg_str(a, {"content", "text"}, &has);
        if (path.empty()) return fail("missing 'path'");
        if (!has) return fail("missing 'content'");
        std::string p = expand_home(path), err;
        bool existed = is_file(p);
        if (!write_file(p, content, err)) return fail(err);
        ToolOutput o;
        o.text = fmt("%s %s (%s)", existed ? "Overwrote" : "Created", path.c_str(),
                     plural(count_lines(content), "line").c_str());
        o.summary = std::string(existed ? "overwrote · " : "created · ") + plural(count_lines(content), "line");
        return o;
    }
    if (name == "edit") {
        bool has_new = false;
        std::string nw = arg_str(a, {"new_string", "new"}, &has_new);
        if (!has_new) return fail("missing 'new_string'");
        return run_edit(path, arg_str(a, {"old_string", "old"}), nw);
    }
    return fail("unknown tool '" + name + "'; available: bash, read, write, edit");
}
