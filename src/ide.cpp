#include "ide.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "json.hpp"
#include "util.hpp"

static const char* socket_path() {
    const char* p = getenv("SFIDE_SOCKET");
    return p && *p ? p : nullptr;
}

bool ide_attached() {
    return socket_path() != nullptr;
}

// One request per connection: a JSON line out, a JSON line back.
static bool call(const Json& req, Json& resp, std::string& err) {
    const char* path = socket_path();
    if (!path) return false;
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) return err = "SFIDE_SOCKET path too long", false;
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return err = strerror(errno), false;
    timeval tv = {30, 0};  // the editor answers between frames; never hang forever
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
        err = std::string("cannot reach SF-IDE: ") + strerror(errno);
        close(fd);
        return false;
    }
    std::string out = req.dump() + "\n";
    for (size_t off = 0; off < out.size();) {
        ssize_t n = write(fd, out.data() + off, out.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            err = "SF-IDE closed the connection";
            close(fd);
            return false;
        }
        off += (size_t)n;
    }
    std::string in;
    char buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        in.append(buf, (size_t)n);
        if (in.back() == '\n') break;
    }
    close(fd);
    if (!Json::parse(in, resp, &err) || !resp.is_obj()) {
        if (err.empty()) err = "bad reply from SF-IDE";
        return false;
    }
    return true;
}

static std::string absolute(const std::string& p) {
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf)) return buf;
    return p.empty() || p[0] == '/' ? p : cwd() + "/" + p;  // a file that does not exist yet
}

bool ide_read(const std::string& path, std::string& data) {
    if (!ide_attached()) return false;
    Json req = Json::object(), resp;
    req.set("op", "read");
    req.set("path", absolute(path));
    std::string err;
    if (!call(req, resp, err) || !resp["ok"].boolean() || !resp["open"].boolean()) return false;
    data = resp["text"].str();
    return true;
}

bool ide_write(const std::string& path, const std::string& data, std::string& err) {
    if (!ide_attached()) return false;
    Json req = Json::object(), resp;
    req.set("op", "write");
    req.set("path", absolute(path));
    req.set("text", data);
    std::string e;
    if (!call(req, resp, e)) return false;  // editor gone: fall back to writing the file
    if (!resp["ok"].boolean()) {
        err = resp["error"].str("SF-IDE could not apply the change");
        return true;
    }
    return true;
}

std::string ide_context() {
    if (!ide_attached()) return "";
    Json req = Json::object(), resp;
    req.set("op", "state");
    std::string err;
    if (!call(req, resp, err) || !resp["ok"].boolean()) return "";
    std::string s;
    std::string active = resp["active"].str();
    if (!active.empty()) {
        s += "active file " + active + " (line " + std::to_string(resp["line"].integer()) + ")";
        if (resp["sel_from"].integer() > 0)
            s += fmt(", selected lines %lld-%lld", resp["sel_from"].integer(), resp["sel_to"].integer());
    }
    std::string others;
    for (auto& f : resp["open"].items())
        if (f.str() != active) others += (others.empty() ? "" : ", ") + f.str();
    if (!others.empty()) s += (s.empty() ? "" : "; ") + std::string("other open tabs: ") + others;
    std::string dirty;
    for (auto& f : resp["unsaved"].items())
        dirty += (dirty.empty() ? "" : ", ") + f.str();
    if (!dirty.empty()) s += "; unsaved: " + dirty;
    long long errors = resp["errors"].integer(), warnings = resp["warnings"].integer();
    if (errors || warnings) s += fmt("; last build: %lld error(s), %lld warning(s)", errors, warnings);
    if (s.empty()) return "";
    std::string out = "\n\n[SF-IDE: " + s + "]";
    std::string sel = resp["selection"].str();
    if (!sel.empty()) out += "\nSelected text:\n```\n" + utf8_clip(sel, 2000) + "\n```";
    return out;
}
