// Tiny HTTP/1.1 client over POSIX sockets: no TLS, no libcurl.
// LM Studio serves plain HTTP on localhost, so this is all we need.
#pragma once
#include <functional>
#include <string>

struct Endpoint {
    std::string host = "127.0.0.1";
    std::string port = "1234";
    std::string token;  // optional bearer token (LM_API_TOKEN)
    std::string str() const { return host + ":" + port; }
};

// Accepts "host", "host:port", "[::1]:port" and "http://host:port/...".
bool parse_endpoint(const std::string& in, Endpoint& ep, std::string& err);

struct HttpResult {
    int status = 0;    // 0 means transport error, see `error`
    std::string body;  // whole body, unless streamed into a sink
    std::string error;
    bool interrupted = false;  // ^C while waiting
    bool ok() const { return status >= 200 && status < 300 && error.empty() && !interrupted; }
};

struct HttpOptions {
    int idle_timeout_ms = 15000;  // give up after this long without bytes (0 = wait forever)
    // Receives the body of a 2xx response as it arrives; return false to stop early.
    std::function<bool(const char*, size_t)> sink;
    std::function<void()> tick;  // called ~10x/s while waiting (spinners)
    const char* accept = "application/json";
};

HttpResult http_request(const Endpoint& ep, const char* method, const std::string& path, const std::string& body,
                        const HttpOptions& opt = {});

// Human-readable message from an error response (LM Studio / OpenAI error shapes).
std::string http_error_text(const HttpResult& r);
