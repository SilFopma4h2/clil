#include "tools.hpp"
#include "mcp.hpp"
#include "util.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <regex.h>
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
{"type":"function","function":{"name":"search","description":"Search file contents recursively with a regex (POSIX extended). Without pattern, list files matching glob. Skips .git, node_modules, build and binary files.","parameters":{"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string","description":"directory or file, default ."},"glob":{"type":"string","description":"file name filter, e.g. *.cpp"},"ignore_case":{"type":"boolean"}}}}},
{"type":"function","function":{"name":"write","description":"Create or overwrite a file.","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}}},
{"type":"function","function":{"name":"insert","description":"Insert text after a line number (0 = top of file). Preferred way to ADD code: no old_string needed. Line numbers come from read.","parameters":{"type":"object","properties":{"path":{"type":"string"},"line":{"type":"integer","description":"insert after this line; 0 = at the start"},"text":{"type":"string"}},"required":["path","line","text"]}}},
{"type":"function","function":{"name":"edit","description":"Replace one exact, unique occurrence of old_string in a file with new_string. Use a short old_string (1-5 lines), without line-number prefixes. To only add code, use insert.","parameters":{"type":"object","properties":{"path":{"type":"string"},"old_string":{"type":"string"},"new_string":{"type":"string"}},"required":["path","old_string","new_string"]}}}
])";

Json tool_schemas() {
    Json j;
    Json::parse(kSchemas, j);
    for (auto& t : mcp_schemas().items())
        j.push(t);
    return j;
}

bool tool_mutates(const std::string& n) {
    return n == "bash" || n == "write" || n == "edit" || n == "insert" || mcp_owns(n);
}
bool tool_known(const std::string& n) {
    return n == "bash" || n == "read" || n == "search" || n == "write" || n == "edit" || n == "insert" || mcp_owns(n);
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
    if (mcp_owns(name)) return a.dump();
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
    if (name == "search") {
        std::string t = arg_str(a, {"pattern", "query"}), g = arg_str(a, {"glob", "include"});
        return t + (g.empty() ? "" : "  " + g) + (path.empty() ? "" : "  in " + path);
    }
    if (name == "write") return path + " (" + plural(count_lines(arg_str(a, {"content", "text"})), "line") + ")";
    if (name == "insert") return path + fmt(" after line %ld", arg_long(a, "line", 0));
    return path;
}

std::vector<std::string> tool_change_preview(const std::string& name, const Json& a) {
    if (name == "write" || name == "insert") return marked(arg_str(a, {"content", "text"}), "+ ", 8);
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
        o.text += fmt("%ld│", i + 1) + l + "\n";
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

// What the file really contains near the intended spot, so the model can retry at once.
static std::string file_context(const std::string& data, const std::string& old_s, size_t cap) {
    if (data.size() <= cap) return data;
    auto lines = split_lines(data);
    size_t hit = 0;
    for (auto& l : split_lines(old_s)) {
        std::string t = trim(l);
        if (t.empty()) continue;
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].find(t) != std::string::npos) {
                hit = i;
                break;
            }
        break;
    }
    size_t from = hit > 15 ? hit - 15 : 0;
    std::string out = fmt("[lines %zu+ of %zu]\n", from + 1, lines.size());
    for (size_t i = from; i < lines.size() && out.size() + lines[i].size() < cap; ++i)
        out += lines[i] + "\n";
    return out;
}

static ToolOutput edit_fail(const std::string& path, const std::string& why, const std::string& data,
                            const std::string& old_s, size_t cap) {
    ToolOutput o;
    o.error = true;
    o.summary = why;
    o.text = "Error: " + why + ". The file was NOT changed. Current content of " + path +
             " (copy old_string exactly from it and call edit again):\n" + utf8_clean(file_context(data, old_s, cap));
    return o;
}

namespace {
struct Backup {
    std::string path, data;
    bool existed;
};
std::vector<std::vector<Backup>> g_undo;  // one group per turn that changed files, oldest first
bool g_new_turn = true;

void backup(const std::string& p) {
    if (g_new_turn) {
        g_undo.emplace_back();
        g_new_turn = false;
        if (g_undo.size() > 10) g_undo.erase(g_undo.begin());
    }
    for (auto& b : g_undo.back())
        if (b.path == p) return;  // keep the state from before the turn
    Backup b{p, "", is_file(p)};
    std::string err;
    if (b.existed) read_file(p, b.data, err);
    g_undo.back().push_back(b);
}
}  // namespace

void tool_new_turn() {
    g_new_turn = true;
}

std::vector<std::string> tool_undo(std::string& err) {
    std::vector<std::string> done;
    if (g_undo.empty()) return done;
    std::vector<Backup> group = std::move(g_undo.back());
    g_undo.pop_back();
    g_new_turn = true;
    for (auto& b : group) {
        std::string e;
        if (b.existed ? write_file(b.path, b.data, e) : unlink(b.path.c_str()) == 0 || errno == ENOENT)
            done.push_back(b.path);
        else
            err += (err.empty() ? "" : "; ") + (e.empty() ? b.path + ": " + strerror(errno) : e);
    }
    return done;
}

// Models copy read's "12│" line-number prefixes into their edits; drop them when every line has one.
static std::string strip_line_numbers(const std::string& s) {
    auto lines = split_lines(s);
    std::string out;
    size_t seen = 0;
    for (auto& l : lines) {
        size_t i = 0;
        while (i < l.size() && isdigit((unsigned char)l[i])) ++i;
        bool has = i > 0 && l.compare(i, 3, "│") == 0;
        if (!has && !trim(l).empty()) return s;
        if (has) ++seen;
        out += has ? l.substr(i + 3) : l;
        out += '\n';
    }
    if (!seen) return s;
    if (s.back() != '\n') out.pop_back();
    return out;
}

ToolOutput run_insert(const std::string& path, long line, const std::string& text, size_t cap) {
    std::string p = expand_home(path);
    if (p.empty()) return fail("missing 'path'");
    if (text.empty()) return fail("missing 'text'");
    std::string data, err;
    if (!read_file(p, data, err)) return fail(err + "; the file was NOT changed");
    auto lines = split_lines(data);
    if (line < 0 || line > (long)lines.size())
        return fail(fmt("line %ld is out of range: %s has %zu lines. The file was NOT changed. Use read to get line numbers",
                        line, path.c_str(), lines.size()));
    std::string ins = strip_line_numbers(text);
    if (ins.back() != '\n') ins += '\n';
    size_t at = 0;  // byte offset just after line `line`
    for (long i = 0; i < line; ++i) {
        size_t nl = data.find('\n', at);
        at = nl == std::string::npos ? data.size() : nl + 1;
    }
    if (at == data.size() && at > 0 && data.back() != '\n') ins = "\n" + ins;
    data.insert(at, ins);
    backup(p);
    if (!write_file(p, data, err)) return fail(err);
    (void)cap;
    ToolOutput o;
    o.text = fmt("Inserted %zu lines into %s after line %ld", count_lines(ins), path.c_str(), line);
    o.summary = fmt("after line %ld: +%zu lines", line, count_lines(ins));
    return o;
}

ToolOutput run_edit(const std::string& path, const std::string& old_in, const std::string& new_in, size_t cap) {
    std::string old_s = strip_line_numbers(old_in), new_s = strip_line_numbers(new_in);
    std::string p = expand_home(path);
    if (p.empty()) return fail("missing 'path'");
    if (old_s.empty()) return fail("old_string is empty; use write to create a file");
    std::string data, err;
    if (!read_file(p, data, err)) return fail(err + "; the file was NOT changed");

    size_t at = std::string::npos, n = 0;
    for (size_t f = data.find(old_s); f != std::string::npos; f = data.find(old_s, f + 1))
        if (++n == 1) at = f;
    if (n > 1)
        return edit_fail(path, fmt("old_string occurs %zu times; include more surrounding lines to make it unique", n),
                         data, old_s, cap);

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
            return edit_fail(path,
                             hits ? fmt("old_string matches %zu places; include more lines", hits)
                                  : std::string("old_string not found in the file"),
                             data, old_s, cap);
        at = starts[hit];
        size_t e = data.find('\n', starts[hit + k - 1]);
        len = (e == std::string::npos ? data.size() : e) - at;
        while (!repl.empty() && repl.back() == '\n')
            repl.pop_back();
        while (!repl.empty() && repl.front() == '\n')
            repl.erase(0, 1);
    }
    data.replace(at, len, repl);
    backup(p);
    if (!write_file(p, data, err)) return fail(err);
    size_t line = 1 + (size_t)std::count(data.begin(), data.begin() + (long)at, '\n');
    ToolOutput o;
    o.text = fmt("Edited %s at line %zu (-%zu +%zu lines)", path.c_str(), line, count_lines(old_s), count_lines(new_s));
    o.summary = fmt("line %zu: -%zu +%zu", line, count_lines(old_s), count_lines(new_s));
    return o;
}

// ------------------------------------------------------------------ search

namespace {
struct Search {
    regex_t re;
    bool has_re = false;
    std::string glob, out;
    size_t cap, matches = 0, files = 0;
    bool full = false;
};

const size_t kMaxMatches = 100;

void search_file(Search& s, const std::string& path) {
    if (!s.glob.empty()) {
        size_t sl = path.rfind('/');
        if (fnmatch(s.glob.c_str(), path.c_str() + (sl == std::string::npos ? 0 : sl + 1), 0) != 0) return;
    }
    if (!s.has_re) {
        ++s.matches;
        s.out += path + "\n";
        return;
    }
    std::string data, err;
    if (!read_file(path, data, err) || data.size() > (2u << 20)) return;
    if (memchr(data.data(), 0, std::min<size_t>(data.size(), 8192))) return;
    ++s.files;
    size_t ln = 0;
    for (auto& line : split_lines(data)) {
        ++ln;
        if (regexec(&s.re, line.c_str(), 0, nullptr, 0) != 0) continue;
        s.out += path + ":" + std::to_string(ln) + ":" + utf8_clip(utf8_clean(line), 200) + "\n";
        if (++s.matches >= kMaxMatches || s.out.size() >= s.cap) {
            s.full = true;
            return;
        }
    }
}

void search_dir(Search& s, const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        if (s.full || g_interrupted) return;
        if (n == "." || n == ".." || n == ".git" || n == "node_modules" || n == "build") continue;
        std::string p = dir == "." ? n : dir + (dir.back() == '/' ? "" : "/") + n;
        if (is_dir(p))
            search_dir(s, p);
        else if (is_file(p))
            search_file(s, p);
    }
}
}  // namespace

// POSIX regex lacks \d; rewrite it (shortcut: wrong inside [...], where \d stays literal-ish).
static std::string expand_digit_class(const std::string& p) {
    std::string r;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\' && i + 1 < p.size()) {
            if (p[i + 1] == 'd')
                r += "[0-9]";
            else
                r += p.substr(i, 2);
            ++i;
        } else
            r += p[i];
    }
    return r;
}

ToolOutput run_search(const std::string& pattern, const std::string& path, const std::string& glob, bool icase,
                      size_t cap) {
    Search s;
    s.cap = cap;
    s.glob = glob;
    if (!pattern.empty()) {
        int flags = REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0);
#ifdef REG_ENHANCED
        flags |= REG_ENHANCED;  // macOS: \w \s \b
#endif
        int rc = regcomp(&s.re, expand_digit_class(pattern).c_str(), flags);
        if (rc != 0) {
            char msg[128];
            regerror(rc, &s.re, msg, sizeof msg);
            return fail(std::string("invalid regex: ") + msg);
        }
        s.has_re = true;
    }
    std::string p = path.empty() ? "." : expand_home(path);
    if (is_dir(p))
        search_dir(s, p);
    else if (is_file(p))
        search_file(s, p);
    else {
        if (s.has_re) regfree(&s.re);
        return fail(p + ": no such file or directory");
    }
    if (s.has_re) regfree(&s.re);
    ToolOutput o;
    o.text = head_tail(s.out, cap);
    if (s.full) o.text += fmt("\n[stopped at %zu matches; narrow the pattern or path]", s.matches);
    if (s.matches == 0) o.text = "(no matches)";
    o.summary = s.has_re ? plural(s.matches, "match", "matches") + " in " + plural(s.files, "file")
                         : plural(s.matches, "file");
    auto lines = split_lines(o.text);
    for (size_t i = 0; i < lines.size() && i < 6; ++i)
        o.preview.push_back(utf8_clip(lines[i], 300));
    if (lines.size() > 6) o.preview.push_back(fmt("… %zu more lines", lines.size() - 6));
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
    if (mcp_owns(name)) return mcp_call(name, a, cap, timeout_s);
    if (name == "bash") return run_bash(arg_str(a, {"command", "cmd"}), cap, timeout_s);
    std::string path = arg_str(a, {"path", "file_path", "file"});
    if (name == "read") return run_read(path, arg_long(a, "offset", 0), arg_long(a, "limit", 0), cap);
    if (name == "search") {
        bool ic = a["ignore_case"].boolean();
        return run_search(arg_str(a, {"pattern", "query"}), path, arg_str(a, {"glob", "include"}), ic, cap);
    }
    if (name == "write") {
        bool has = false;
        std::string content = arg_str(a, {"content", "text"}, &has);
        if (!content.empty() && content.back() != '\n') content += '\n';  // text files end with a newline
        if (path.empty()) return fail("missing 'path'");
        if (!has) return fail("missing 'content'");
        std::string p = expand_home(path), err;
        bool existed = is_file(p);
        backup(p);
        if (!write_file(p, content, err)) return fail(err);
        ToolOutput o;
        o.text = fmt("%s %s (%s)", existed ? "Overwrote" : "Created", path.c_str(),
                     plural(count_lines(content), "line").c_str());
        o.summary = std::string(existed ? "overwrote · " : "created · ") + plural(count_lines(content), "line");
        return o;
    }
    if (name == "insert") {
        bool has = false;
        std::string text = arg_str(a, {"text", "content", "new_string"}, &has);
        if (!has) return fail("missing 'text'");
        if (!a["line"].is_num() && !a["line"].is_str()) return fail("missing 'line' (insert after this line number; 0 = start)");
        return run_insert(path, arg_long(a, "line", 0), text, cap);
    }
    if (name == "edit") {
        bool has_new = false;
        std::string nw = arg_str(a, {"new_string", "new"}, &has_new);
        if (!has_new) return fail("missing 'new_string'");
        return run_edit(path, arg_str(a, {"old_string", "old"}), nw, cap);
    }
    return fail("unknown tool '" + name + "'; available: bash, read, search, write, edit, insert");
}
