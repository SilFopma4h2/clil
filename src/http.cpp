#include "http.hpp"
#include "json.hpp"
#include "util.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

bool parse_endpoint(const std::string& in, Endpoint& ep, std::string& err) {
    std::string s = trim(in);
    size_t k = s.find("://");
    if (k != std::string::npos) {
        std::string scheme = lower(s.substr(0, k));
        if (scheme != "http") {
            err = "only http:// is supported (LM Studio serves plain HTTP)";
            return false;
        }
        s = s.substr(k + 3);
    }
    size_t slash = s.find('/');
    if (slash != std::string::npos) s.resize(slash);
    if (s.empty()) {
        err = "empty host";
        return false;
    }
    std::string host = s, port = ep.port;
    if (s[0] == '[') {
        size_t rb = s.find(']');
        if (rb == std::string::npos) {
            err = "bad IPv6 address";
            return false;
        }
        host = s.substr(1, rb - 1);
        if (rb + 1 < s.size() && s[rb + 1] == ':') port = s.substr(rb + 2);
    } else if (s.find(':') != std::string::npos && s.find(':') == s.rfind(':')) {
        size_t c = s.find(':');
        host = s.substr(0, c);
        port = s.substr(c + 1);
    }
    if (host.empty()) host = "127.0.0.1";
    if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) {
        err = "bad port in '" + in + "'";
        return false;
    }
    ep.host = host;
    ep.port = port;
    return true;
}

static int connect_to(const Endpoint& ep, int timeout_ms, std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int g = getaddrinfo(ep.host.c_str(), ep.port.c_str(), &hints, &res);
    if (g) {
        err = "cannot resolve " + ep.host + ": " + gai_strerror(g);
        return -1;
    }
    int fd = -1;
    std::string why = "no address";
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            int pr;
            do
                pr = poll(&pfd, 1, timeout_ms);
            while (pr < 0 && errno == EINTR && !g_interrupted);
            if (pr == 1) {
                int e = 0;
                socklen_t l = sizeof e;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
                r = e ? (errno = e, -1) : 0;
            } else {
                if (pr == 0) errno = ETIMEDOUT;
                r = -1;
            }
        }
        if (r == 0) break;
        why = strerror(errno);
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        err = "cannot connect to " + ep.str() + " (" + why + ")";
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

static bool send_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = send(fd, data.data() + off, data.size() - off, 0);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (g_interrupted) return false;
            pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, 30000) == 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

namespace {
// Incremental decoder for Transfer-Encoding: chunked.
struct Chunked {
    enum State { Size, Data, DataEnd, Done } st = Size;
    size_t left = 0;
    std::string raw;

    // Appends decoded bytes to `out`; returns false on a malformed stream.
    bool feed(const char* d, size_t n, std::string& out) {
        raw.append(d, n);
        size_t pos = 0;
        for (;;) {
            if (st == Size) {
                size_t nl = raw.find("\r\n", pos);
                if (nl == std::string::npos) break;
                char* e = nullptr;
                unsigned long v = strtoul(raw.c_str() + pos, &e, 16);
                if (e == raw.c_str() + pos) return false;
                pos = nl + 2;
                if (v == 0) {
                    st = Done;
                    break;
                }
                left = v;
                st = Data;
            } else if (st == Data) {
                size_t take = std::min(left, raw.size() - pos);
                out.append(raw, pos, take);
                pos += take;
                left -= take;
                if (left) break;
                st = DataEnd;
            } else if (st == DataEnd) {
                if (raw.size() - pos < 2) break;
                pos += 2;
                st = Size;
            } else
                break;
        }
        raw.erase(0, pos);
        return true;
    }
};
}  // namespace

HttpResult http_request(const Endpoint& ep, const char* method, const std::string& path, const std::string& body,
                        const HttpOptions& opt) {
    HttpResult r;
    int fd = connect_to(ep, 5000, r.error);
    if (fd < 0) {
        r.interrupted = g_interrupted;
        return r;
    }
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + ep.str() +
                      "\r\nUser-Agent: clil\r\nAccept: " + opt.accept + "\r\nConnection: close\r\n";
    if (!ep.token.empty()) req += "Authorization: Bearer " + ep.token + "\r\n";
    if (!body.empty() || strcmp(method, "POST") == 0)
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    req += body;
    if (!send_all(fd, req)) {
        r.interrupted = g_interrupted;
        r.error = "failed to send request to " + ep.str();
        close(fd);
        return r;
    }

    std::string head, decoded;
    bool have_head = false, chunked = false, stop = false;
    long long content_length = -1, received = 0;
    Chunked dec;
    double last_byte = now_secs();
    int status = 0;

    auto deliver = [&](const char* d, size_t n) {
        if (!n) return;
        if (opt.sink && status >= 200 && status < 300) {
            if (!opt.sink(d, n)) stop = true;
        } else if (r.body.size() < (64u << 20)) {
            r.body.append(d, n);
        }
    };
    auto feed_body = [&](const char* d, size_t n) {
        received += (long long)n;
        if (!chunked) return deliver(d, n);
        decoded.clear();
        if (!dec.feed(d, n, decoded)) {
            r.error = "malformed chunked response";
            stop = true;
        }
        deliver(decoded.data(), decoded.size());
    };
    auto body_done = [&]() {
        return have_head && (chunked ? dec.st == Chunked::Done : content_length >= 0 && received >= content_length);
    };

    char buf[16384];
    while (!stop && !body_done()) {
        if (g_interrupted) {
            r.interrupted = true;
            break;
        }
        pollfd pfd{fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            r.error = strerror(errno);
            break;
        }
        if (pr == 0) {
            if (opt.tick) opt.tick();
            if (opt.idle_timeout_ms > 0 && (now_secs() - last_byte) * 1000 > opt.idle_timeout_ms) {
                r.error = "timed out waiting for " + ep.str();
                break;
            }
            continue;
        }
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            r.error = std::string("read failed: ") + strerror(errno);
            break;
        }
        if (n == 0) break;  // EOF
        last_byte = now_secs();
        if (have_head) {
            feed_body(buf, (size_t)n);
            continue;
        }
        head.append(buf, (size_t)n);
        size_t he = head.find("\r\n\r\n");
        if (he == std::string::npos) {
            if (head.size() > (64u << 10)) {
                r.error = "response header too large";
                break;
            }
            continue;
        }
        have_head = true;
        std::string rest = head.substr(he + 4);
        head.resize(he);
        // "HTTP/1.1 200 OK"
        size_t sp = head.find(' ');
        status = sp == std::string::npos ? 0 : atoi(head.c_str() + sp + 1);
        for (auto& line : split_lines(head)) {
            std::string l = lower(line);
            if (starts_with(l, "transfer-encoding:") && l.find("chunked") != std::string::npos)
                chunked = true;
            else if (starts_with(l, "content-length:"))
                content_length = atoll(l.c_str() + 15);
        }
        if (status < 100) {
            r.error = "invalid HTTP response from " + ep.str();
            break;
        }
        r.status = status;
        feed_body(rest.data(), rest.size());
    }
    close(fd);
    if (!have_head && r.error.empty() && !r.interrupted)
        r.error = "connection to " + ep.str() + " closed without a response";
    if (!have_head) r.status = 0;
    return r;
}

std::string http_error_text(const HttpResult& r) {
    if (r.interrupted) return "interrupted";
    if (!r.error.empty()) return r.error;
    if (r.status == 0) return "request failed";
    Json j;
    if (Json::parse(r.body, j)) {
        const Json& e = j["error"];
        if (e.is_str()) return e.str();
        if (e["message"].is_str()) return e["message"].str();
        if (j["message"].is_str()) return j["message"].str();
    }
    std::string b = trim(r.body);
    return fmt("HTTP %d", r.status) + (b.empty() ? "" : ": " + utf8_clip(b, 300));
}
