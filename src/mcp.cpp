#include "mcp.hpp"
#include "state.h"
#include "util.hpp"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {

struct Server {
    std::string name, command;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
    bool supported = true;
    bool share = true;       // run through a host process that other clil's use too
    bool lazy = false;       // tools come from the cache, nothing runs until the first call
    std::string sig;         // hash of the launch config: invalidates the cache and the host
    pid_t pid = -1;          // direct child; -1 when running via a shared host
    int in = -1, out = -1;  // our write end / read end (the same socket for a shared host)
    std::string buf;
    int next_id = 1;
    std::vector<std::pair<std::string, std::string>> tools;  // exposed name -> server's name
};

std::vector<Server> g_servers;
Json g_schemas = Json::array();
int g_gen = 0;

bool up(const Server& s) {
    return s.in >= 0;
}

unsigned long long fnv64(const std::string& x, unsigned long long h = 1469598103934665603ull) {
    for (unsigned char c : x)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

void load_config() {
    std::string text, err;
    Json cfg;
    if (!read_file(home_dir() + "/.lmstudio/mcp.json", text, err) || !Json::parse(text, cfg)) return;
    for (auto& m : cfg["mcpServers"].members()) {
        bool known = false;
        for (auto& s : g_servers)
            known = known || s.name == m.first;
        if (known) continue;
        Server s;
        s.name = m.first;
        s.command = m.second["command"].str();
        s.supported = !s.command.empty();
        s.share = m.second["clil_share"].boolean(true);  // "clil_share": false for servers with per-client state
        unsigned long long h = fnv64(s.command);
        for (auto& a : m.second["args"].items()) {
            s.args.push_back(a.str());
            h = fnv64(a.str(), h * 31 + 1);
        }
        for (auto& e : m.second["env"].members()) {
            s.env.push_back({e.first, e.second.str()});
            h = fnv64(e.first + "=" + e.second.str(), h * 31 + 2);
        }
        s.sig = fmt("%016llx", h);
        g_servers.push_back(s);
    }
}

Server* find(const std::string& name) {
    load_config();
    for (auto& s : g_servers)
        if (s.name == name) return &s;
    return nullptr;
}

bool write_line(int fd, const Json& msg) {
    std::string line = msg.dump() + "\n";
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = write(fd, line.data() + off, line.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

bool send(Server& s, const Json& msg) {
    return write_line(s.in, msg);
}

// Wait for the response to request `id`; answers or ignores whatever else the server sends.
bool recv(Server& s, int id, double timeout_s, Json& result, std::string& err) {
    double t0 = now_secs();
    for (;;) {
        size_t nl = s.buf.find('\n');
        if (nl != std::string::npos) {
            std::string line = s.buf.substr(0, nl);
            s.buf.erase(0, nl + 1);
            Json m;
            if (!Json::parse(line, m) || !m.is_obj()) continue;  // log noise on stdout
            if (m.has("method")) {
                if (m.has("id")) {
                    Json r = Json::object();
                    r.set("jsonrpc", "2.0").set("id", m["id"]);
                    if (m["method"].str() == "ping")
                        r.set("result", Json::object());
                    else
                        r.set("error", Json::object().set("code", -32601).set("message", "method not supported"));
                    send(s, r);
                }
                continue;
            }
            if (m["id"].integer(-1) != id) continue;
            if (m.has("error")) {
                err = m["error"]["message"].str("request failed");
                return false;
            }
            result = m["result"];
            return true;
        }
        if (g_interrupted) return err = "interrupted", false;
        if (now_secs() - t0 > timeout_s) return err = fmt("no response after %.0fs", timeout_s), false;
        pollfd p{s.out, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) {
            char b[8192];
            ssize_t n = read(s.out, b, sizeof b);
            if (n > 0)
                s.buf.append(b, (size_t)n);
            else if (n == 0 || errno != EINTR)
                return err = "server exited", false;
        }
    }
}

bool request(Server& s, const char* method, Json params, double timeout_s, Json& result, std::string& err) {
    int id = s.next_id++;
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("id", id).set("method", method).set("params", std::move(params));
    if (!send(s, m)) return err = "server exited", false;
    return recv(s, id, timeout_s, result, err);
}

// Closes the connection; a shared host keeps running for the other clil's (it exits when idle).
void stop(Server& s) {
    if (s.in >= 0) close(s.in);
    if (s.out >= 0 && s.out != s.in) close(s.out);
    s.in = s.out = -1;
    if (s.pid > 0) {
        kill(-s.pid, SIGTERM);
        int st = 0;
        for (int i = 0; i < 40 && waitpid(s.pid, &st, WNOHANG) == 0; ++i)
            usleep(50000);
        if (waitpid(s.pid, &st, WNOHANG) == 0) {
            kill(-s.pid, SIGKILL);
            waitpid(s.pid, &st, 0);
        }
    }
    s.pid = -1;
    s.buf.clear();
}

bool spawn(Server& s, std::string& err) {
    int to[2], from[2];
    if (pipe(to) < 0) return err = strerror(errno), false;
    if (pipe(from) < 0) return close(to[0]), close(to[1]), err = strerror(errno), false;
    std::vector<char*> av;
    av.push_back(const_cast<char*>(s.command.c_str()));
    for (auto& a : s.args)
        av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return err = strerror(errno), false;
    if (pid == 0) {
        setpgid(0, 0);  // ^C at the prompt must not kill the server
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        dup2(to[0], 0);
        dup2(from[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        close(to[0]), close(to[1]), close(from[0]), close(from[1]);
        for (auto& e : s.env)
            setenv(e.first.c_str(), e.second.c_str(), 1);
        execvp(av[0], av.data());
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    s.pid = pid;
    s.in = to[1];
    s.out = from[0];
    fcntl(s.in, F_SETFD, FD_CLOEXEC);  // keep bash tool children from inheriting the pipes
    fcntl(s.out, F_SETFD, FD_CLOEXEC);
    return true;
}

std::string exposed_name(const std::string& server, const std::string& tool) {
    std::string n = server + "__" + tool;
    for (auto& c : n)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') c = '_';
    return n.substr(0, 64);
}

Json init_params() {
    Json p = Json::object();
    p.set("protocolVersion", "2025-06-18")
        .set("capabilities", Json::object())
        .set("clientInfo", Json::object().set("name", "clil").set("version", "0.1.0"));
    return p;
}

void notify_initialized(Server& s) {
    Json note = Json::object();
    note.set("jsonrpc", "2.0").set("method", "notifications/initialized");
    send(s, note);
}

// ---------------------------------------------------------------- shared host

std::string sock_path(const Server& s) {
    std::string d = clil_dir();
    return d.empty() ? "" : d + "/mcp." + fmt("%016llx", fnv64(s.name + s.sig)) + ".sock";
}

int dial(const std::string& path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path.c_str(), path.size());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0) return close(fd), -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

std::string self_exe() {
    char b[PATH_MAX];
#ifdef __APPLE__
    uint32_t n = sizeof b;
    return _NSGetExecutablePath(b, &n) == 0 ? b : "";
#else
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    return n > 0 ? std::string(b, (size_t)n) : "";
#endif
}

// Connects to the host of this server, starting it first if no clil has yet.
bool attach(Server& s, std::string& err) {
    std::string sock = sock_path(s), exe = self_exe();
    if (sock.empty() || exe.empty()) return err = "no run directory", false;
    int fd = dial(sock);
    if (fd < 0) {
        int lock = clil_lock(("mcp " + sock).c_str(), [](void*, int) { return g_interrupted ? 1 : 0; }, nullptr);
        if (lock == -1) return err = "interrupted", false;
        fd = dial(sock);  // another clil may have started it while we waited
        if (fd < 0) {
            fflush(stdout);
            fflush(stderr);
            pid_t pid = fork();
            if (pid == 0) {
                int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) dup2(dn, 0), dup2(dn, 1), dup2(dn, 2);
                execl(exe.c_str(), exe.c_str(), "--mcp-host", s.name.c_str(), (char*)nullptr);
                _exit(127);
            }
            // The first run of an npx/uvx server may download packages.
            for (int i = 0; pid > 0 && fd < 0; ++i) {
                if (g_interrupted || i > 1800 || waitpid(pid, nullptr, WNOHANG) == pid) break;
                usleep(50000);
                fd = dial(sock);
            }
        }
        clil_unlock(lock);
    }
    if (fd < 0) return err = "cannot start the shared server", false;
    s.in = s.out = fd;
    return true;
}

volatile std::sig_atomic_t g_term = 0;

}  // namespace

int mcp_host_main(const std::string& name) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, [](int) { g_term = 1; });
    setsid();
    clil_open(nullptr);  // for the run directory
    Server* sp = find(name);
    if (!sp || !sp->supported) return 1;
    Server& s = *sp;
    std::string err;
    Json init;
    if (!spawn(s, err)) return 1;
    if (!request(s, "initialize", init_params(), 60, init, err)) return stop(s), 1;
    notify_initialized(s);

    std::string sock = sock_path(s);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0 || sock.empty() || sock.size() >= sizeof a.sun_path) return stop(s), 1;
    memcpy(a.sun_path, sock.c_str(), sock.size());
    unlink(sock.c_str());
    umask(077);
    if (bind(ls, (sockaddr*)&a, sizeof a) < 0 || listen(ls, 8) < 0) return stop(s), 1;

    struct Client {
        int fd;
        std::string buf;
    };
    struct Pending {
        long long up;
        int fd;
        Json id;
    };
    std::vector<Client> cl;
    std::vector<Pending> pend;
    const char* idle_env = getenv("CLIL_MCP_IDLE");
    double idle_s = idle_env ? atof(idle_env) : 300, last = now_secs();
    char b[8192];

    while (!g_term) {
        std::vector<pollfd> pf = {{ls, POLLIN, 0}, {s.out, POLLIN, 0}};
        for (auto& c : cl)
            pf.push_back({c.fd, POLLIN, 0});
        if (poll(pf.data(), pf.size(), 1000) < 0 && errno != EINTR) break;
        if (!cl.empty())
            last = now_secs();
        else if (now_secs() - last > idle_s)
            break;
        if (pf[0].revents & POLLIN) {
            int fd = accept(ls, nullptr, nullptr);
            if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC), cl.push_back({fd, ""}), last = now_secs();
        }
        if (pf[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(s.out, b, sizeof b);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) break;  // the server died
            if (n > 0) s.buf.append(b, (size_t)n);
        }
        for (size_t nl; (nl = s.buf.find('\n')) != std::string::npos;) {
            Json m;
            bool ok = Json::parse(s.buf.substr(0, nl), m) && m.is_obj();
            s.buf.erase(0, nl + 1);
            if (!ok) continue;
            if (m.has("method")) {  // the server asks us something: like a plain client, say no
                if (m.has("id")) {
                    Json r = Json::object();
                    r.set("jsonrpc", "2.0").set("id", m["id"]);
                    if (m["method"].str() == "ping")
                        r.set("result", Json::object());
                    else
                        r.set("error", Json::object().set("code", -32601).set("message", "method not supported"));
                    send(s, r);
                }
                continue;
            }
            for (size_t k = 0; k < pend.size(); ++k)
                if (pend[k].up == m["id"].integer(-1)) {
                    m.set("id", pend[k].id);
                    write_line(pend[k].fd, m);
                    pend.erase(pend.begin() + (long)k);
                    break;
                }
        }
        for (size_t i = pf.size() - 2; i-- > 0;) {  // clients that were polled, newest first
            if (!(pf[i + 2].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            Client& c = cl[i];
            ssize_t n = read(c.fd, b, sizeof b);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) {
                for (size_t k = pend.size(); k-- > 0;)
                    if (pend[k].fd == c.fd) pend.erase(pend.begin() + (long)k);
                close(c.fd);
                cl.erase(cl.begin() + (long)i);
                continue;
            }
            c.buf.append(b, (size_t)n);
            for (size_t nl; (nl = c.buf.find('\n')) != std::string::npos;) {
                Json m;
                bool ok = Json::parse(c.buf.substr(0, nl), m) && m.is_obj() && m.has("id");  // no notifications
                c.buf.erase(0, nl + 1);
                if (!ok) continue;
                const std::string& method = m["method"].str();
                if (method == "initialize" || method == "ping") {
                    Json r = Json::object();
                    r.set("jsonrpc", "2.0").set("id", m["id"]).set("result", method == "ping" ? Json::object() : init);
                    write_line(c.fd, r);
                } else {
                    long long id = s.next_id++;
                    pend.push_back({id, c.fd, m["id"]});
                    m.set("id", id);
                    send(s, m);
                }
            }
        }
    }
    for (auto& c : cl)
        close(c.fd);
    close(ls);
    unlink(sock.c_str());
    stop(s);
    return 0;
}

namespace {

// ---------------------------------------------------------------- tool cache

std::string cache_path() {
    return home_dir() + "/.clil/mcp-cache.json";
}

bool cache_get(const Server& s, Json& tools) {
    std::string text, err;
    Json j;
    if (!read_file(cache_path(), text, err) || !Json::parse(text, j)) return false;
    if (j[s.name]["sig"].str() != s.sig || !j[s.name]["tools"].is_arr()) return false;
    tools = j[s.name]["tools"];
    return true;
}

void cache_put(const Server& s, const Json& tools) {
    std::string text, err;
    Json j;
    if (!read_file(cache_path(), text, err) || !Json::parse(text, j) || !j.is_obj()) j = Json::object();
    j.set(s.name, Json::object().set("sig", s.sig).set("tools", tools));
    std::string tmp = cache_path() + "." + std::to_string(getpid());
    if (write_file(tmp, j.dump(), err)) rename(tmp.c_str(), cache_path().c_str());
}

void cache_drop(const Server& s) {
    std::string text, err;
    Json j;
    if (!read_file(cache_path(), text, err) || !Json::parse(text, j) || !j.erase(s.name.c_str())) return;
    std::string tmp = cache_path() + "." + std::to_string(getpid());
    if (write_file(tmp, j.dump(), err)) rename(tmp.c_str(), cache_path().c_str());
}

bool fetch_tools(Server& s, Json& tools, std::string& err) {
    tools = Json::array();
    std::string cursor;
    do {
        Json p = Json::object(), res;
        if (!cursor.empty()) p.set("cursor", cursor);
        if (!request(s, "tools/list", p, 30, res, err)) return false;
        for (auto& t : res["tools"].items())
            tools.push(t);
        cursor = res["nextCursor"].str();
    } while (!cursor.empty());
    return true;
}

void register_tools(Server& s, const Json& tools) {
    for (auto& t : tools.items()) {
        std::string real = t["name"].str(), ex = exposed_name(s.name, real);
        if (real.empty()) continue;
        s.tools.push_back({ex, real});
        Json fn = Json::object(), schema = t["inputSchema"];
        if (!schema.is_obj()) schema = Json::object().set("type", "object");
        fn.set("name", ex).set("description", utf8_clip(t["description"].str(), 240)).set("parameters", schema);
        g_schemas.push(Json::object().set("type", "function").set("function", fn));
    }
}

// Starts (or joins) the server and does the MCP handshake.
bool open_conn(Server& s, std::string& err) {
    static bool once = (signal(SIGPIPE, SIG_IGN), atexit(mcp_shutdown), true);
    (void)once;
    const char* off = getenv("CLIL_MCP_SHARE");
    std::string why;
    if (!(s.share && !(off && !strcmp(off, "0")) && attach(s, why)) && !spawn(s, err)) return false;
    Json res;
    if (!request(s, "initialize", init_params(), 60, res, err)) return stop(s), false;
    notify_initialized(s);
    return true;
}

}  // namespace

std::vector<McpInfo> mcp_list() {
    load_config();
    std::vector<McpInfo> out;
    for (auto& s : g_servers)
        out.push_back({s.name, up(s) || s.lazy, s.supported, s.tools.size()});
    return out;
}

bool mcp_connect(const std::string& name, std::string& err) {
    Server* sp = find(name);
    if (!sp) return err = "no server '" + name + "' in ~/.lmstudio/mcp.json", false;
    Server& s = *sp;
    if (!s.supported) return err = "only stdio servers (command/args) are supported", false;
    if (up(s) || s.lazy) return err = "already connected", false;
    Json tools;
    if (cache_get(s, tools)) {  // same launch config as last time: start the server on the first call
        s.lazy = true;
        register_tools(s, tools);
        ++g_gen;
        return true;
    }
    if (!open_conn(s, err)) return false;
    if (!fetch_tools(s, tools, err)) return stop(s), false;
    register_tools(s, tools);
    cache_put(s, tools);
    ++g_gen;
    return true;
}

bool mcp_disconnect(const std::string& name) {
    Server* s = find(name);
    if (!s || !(up(*s) || s->lazy)) return false;
    Json keep = Json::array();
    for (auto& e : g_schemas.items()) {
        bool mine = false;
        for (auto& t : s->tools)
            mine = mine || t.first == e["function"]["name"].str();
        if (!mine) keep.push(e);
    }
    g_schemas = keep;
    stop(*s);
    s->tools.clear();
    s->lazy = false;
    ++g_gen;
    return true;
}

void mcp_shutdown() {
    for (auto& s : g_servers)
        if (up(s)) stop(s);
}

bool mcp_owns(const std::string& tool) {
    for (auto& s : g_servers)
        for (auto& t : s.tools)
            if (t.first == tool) return true;
    return false;
}

const Json& mcp_schemas() { return g_schemas; }
int mcp_generation() { return g_gen; }

ToolOutput mcp_call(const std::string& tool, const Json& args, size_t cap, int timeout_s) {
    ToolOutput o;
    o.error = true;
    for (auto& s : g_servers) {
        for (auto& t : s.tools) {
            if (t.first != tool) continue;
            Json params = Json::object(), res;
            std::string err;
            if (!up(s)) {
                if (!open_conn(s, err)) {
                    cache_drop(s);  // /mcp disconnect + connect then starts it eagerly and shows the real error
                    o.text = "Error: MCP server '" + s.name + "' failed to start: " + err;
                    o.summary = "failed to start";
                    return o;
                }
                Json fresh;
                if (fetch_tools(s, fresh, err)) cache_put(s, fresh);  // keeps the next session's cache current
            }
            params.set("name", t.second).set("arguments", args);
            if (!request(s, "tools/call", params, timeout_s > 0 ? timeout_s : 180, res, err)) {
                o.text = "Error: " + err;
                o.summary = err;
                return o;
            }
            std::string text;
            for (auto& c : res["content"].items()) {
                if (!text.empty()) text += "\n";
                text += c["type"].str() == "text" ? c["text"].str() : "[" + c["type"].str("content") + " not shown]";
            }
            if (text.empty() && res.has("structuredContent")) text = res["structuredContent"].dump();
            text = utf8_clean(clean_terminal_output(text));
            auto lines = split_lines(text);
            for (size_t i = 0; i < lines.size() && i < 6; ++i)
                o.preview.push_back(utf8_clip(lines[i], 300));
            if (lines.size() > 6) o.preview.push_back(fmt("… %zu more lines", lines.size() - 6));
            o.error = res["isError"].boolean();
            o.text = head_tail(text, cap);
            if (o.error) o.text = "Error: " + o.text;
            if (o.text.empty()) o.text = "(no output)";
            o.summary = (o.error ? "error · " : "") + plural(lines.size(), "line");
            return o;
        }
    }
    o.text = "Error: MCP tool '" + tool + "' is not connected";
    o.summary = "not connected";
    return o;
}
