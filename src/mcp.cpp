#include "mcp.hpp"
#include "util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

struct Server {
    std::string name, command;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
    bool supported = true;
    pid_t pid = -1;
    int in = -1, out = -1;  // our write end / read end
    std::string buf;
    int next_id = 1;
    std::vector<std::pair<std::string, std::string>> tools;  // exposed name -> server's name
};

std::vector<Server> g_servers;
Json g_schemas = Json::array();
int g_gen = 0;

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
        for (auto& a : m.second["args"].items())
            s.args.push_back(a.str());
        for (auto& e : m.second["env"].members())
            s.env.push_back({e.first, e.second.str()});
        g_servers.push_back(s);
    }
}

Server* find(const std::string& name) {
    load_config();
    for (auto& s : g_servers)
        if (s.name == name) return &s;
    return nullptr;
}

bool send(Server& s, const Json& msg) {
    std::string line = msg.dump() + "\n";
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = write(s.in, line.data() + off, line.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
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

void stop(Server& s) {
    if (s.in >= 0) close(s.in);
    if (s.out >= 0) close(s.out);
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
    s.tools.clear();
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

}  // namespace

std::vector<McpInfo> mcp_list() {
    load_config();
    std::vector<McpInfo> out;
    for (auto& s : g_servers)
        out.push_back({s.name, s.pid > 0, s.supported, s.tools.size()});
    return out;
}

bool mcp_connect(const std::string& name, std::string& err) {
    Server* sp = find(name);
    if (!sp) return err = "no server '" + name + "' in ~/.lmstudio/mcp.json", false;
    Server& s = *sp;
    if (!s.supported) return err = "only stdio servers (command/args) are supported", false;
    if (s.pid > 0) return err = "already connected", false;
    static bool once = (signal(SIGPIPE, SIG_IGN), atexit(mcp_shutdown), true);
    (void)once;
    if (!spawn(s, err)) return false;

    Json params = Json::object(), res;
    params.set("protocolVersion", "2025-06-18")
        .set("capabilities", Json::object())
        .set("clientInfo", Json::object().set("name", "clil").set("version", "0.1.0"));
    bool ok = request(s, "initialize", params, 60, res, err);  // first npx/uvx run may download packages
    if (ok) {
        Json note = Json::object();
        note.set("jsonrpc", "2.0").set("method", "notifications/initialized");
        send(s, note);
        std::string cursor;
        do {
            Json p = Json::object();
            if (!cursor.empty()) p.set("cursor", cursor);
            if (!(ok = request(s, "tools/list", p, 30, res, err))) break;
            for (auto& t : res["tools"].items()) {
                std::string real = t["name"].str(), ex = exposed_name(s.name, real);
                if (real.empty()) continue;
                s.tools.push_back({ex, real});
                Json fn = Json::object(), schema = t["inputSchema"];
                if (!schema.is_obj()) schema = Json::object().set("type", "object");
                fn.set("name", ex).set("description", utf8_clip(t["description"].str(), 240)).set("parameters", schema);
                g_schemas.push(Json::object().set("type", "function").set("function", fn));
            }
            cursor = res["nextCursor"].str();
        } while (!cursor.empty());
    }
    if (!ok) {
        // drop any schemas added before the failure
        Json keep = Json::array();
        for (auto& e : g_schemas.items())
            if (!starts_with(e["function"]["name"].str(), exposed_name(s.name, "").c_str())) keep.push(e);
        g_schemas = keep;
        stop(s);
        return false;
    }
    ++g_gen;
    return true;
}

bool mcp_disconnect(const std::string& name) {
    Server* s = find(name);
    if (!s || s->pid < 0) return false;
    Json keep = Json::array();
    for (auto& e : g_schemas.items()) {
        bool mine = false;
        for (auto& t : s->tools)
            mine = mine || t.first == e["function"]["name"].str();
        if (!mine) keep.push(e);
    }
    g_schemas = keep;
    stop(*s);
    ++g_gen;
    return true;
}

void mcp_shutdown() {
    for (auto& s : g_servers)
        if (s.pid > 0) stop(s);
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
            params.set("name", t.second).set("arguments", args);
            std::string err;
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
