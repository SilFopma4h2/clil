// Unit tests: `make test`. Includes a throwaway local HTTP server to exercise
// streaming (SSE over chunked encoding) without LM Studio.
#include "app.hpp"
#include "http.hpp"
#include "json.hpp"
#include "lmstudio.hpp"
#include "tools.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (cond)                                                           \
            ++g_pass;                                                       \
        else {                                                              \
            ++g_fail;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)
#define CHECK_EQ(a, b)                                                                                        \
    do {                                                                                                      \
        auto va = (a);                                                                                        \
        auto vb = (b);                                                                                        \
        if (va == vb)                                                                                         \
            ++g_pass;                                                                                         \
        else {                                                                                                \
            ++g_fail;                                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s == %s\n  got:  [%s]\n  want: [%s]\n", __FILE__, __LINE__, #a, #b, \
                    std::string(va).c_str(), std::string(vb).c_str());                                        \
        }                                                                                                     \
    } while (0)

// ------------------------------------------------------------------ fake server

struct Server {
    Endpoint ep;
    pid_t pid = -1;
    int req_fd = -1;
    std::string requests() {  // everything the client sent, after the server exits
        int st;
        waitpid(pid, &st, 0);
        std::string s;
        char b[4096];
        ssize_t n;
        while ((n = read(req_fd, b, sizeof b)) > 0)
            s.append(b, (size_t)n);
        close(req_fd);
        return s;
    }
};

// Answers one connection per entry in `responses`, writing each response in the given pieces.
static Server serve(const std::vector<std::vector<std::string>>& responses) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (sockaddr*)&a, sizeof a);
    listen(ls, 4);
    socklen_t al = sizeof a;
    getsockname(ls, (sockaddr*)&a, &al);
    int pfd[2];
    if (pipe(pfd) != 0) abort();
    Server s;
    s.ep.port = std::to_string(ntohs(a.sin_port));
    s.pid = fork();
    if (s.pid == 0) {
        close(pfd[0]);
        for (auto& pieces : responses) {
            int c = accept(ls, nullptr, nullptr);
            std::string req;
            char buf[4096];
            for (;;) {
                ssize_t n = read(c, buf, sizeof buf);
                if (n <= 0) break;
                req.append(buf, (size_t)n);
                size_t he = req.find("\r\n\r\n");
                if (he == std::string::npos) continue;
                size_t cl = 0, p = req.find("Content-Length: ");
                if (p != std::string::npos) cl = (size_t)atoi(req.c_str() + p + 16);
                if (req.size() >= he + 4 + cl) break;
            }
            req += "\n----\n";
            if (write(pfd[1], req.data(), req.size()) < 0) _exit(1);
            for (auto& piece : pieces) {
                if (write(c, piece.data(), piece.size()) < 0) break;
                usleep(1000);
            }
            close(c);
        }
        _exit(0);
    }
    close(ls);
    close(pfd[1]);
    s.req_fd = pfd[0];
    return s;
}

static std::vector<std::string> split_every(const std::string& wire, size_t n) {
    std::vector<std::string> out;
    for (size_t i = 0; i < wire.size(); i += n)
        out.push_back(wire.substr(i, n));
    return out;
}

static std::vector<std::string> sse_response(const std::vector<std::string>& events, size_t chunk, size_t piece) {
    std::string body;
    for (auto& e : events)
        body += "data: " + e + "\n\n";
    body += "data: [DONE]\n\n";
    std::string wire = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (size_t i = 0; i < body.size(); i += chunk) {
        std::string c = body.substr(i, chunk);
        wire += fmt("%zx\r\n", c.size()) + c + "\r\n";
    }
    wire += "0\r\n\r\n";
    return split_every(wire, piece);
}

static std::vector<std::string> json_response(int status, const std::string& body) {
    return {fmt("HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", status, body.size()) +
            body};
}

// ------------------------------------------------------------------ tests

static void test_json() {
    Json j;
    CHECK(Json::parse(R"({"a":1,"b":[true,false,null],"c":"x\ny","d":-2.5,"e":1e3})", j));
    CHECK_EQ(j.dump(), std::string(R"({"a":1,"b":[true,false,null],"c":"x\ny","d":-2.5,"e":1000})"));
    CHECK(j["b"][1].is_bool() && !j["b"][1].boolean(true));
    CHECK(j["missing"]["deeper"][3].is_null());
    CHECK_EQ(j["c"].str(), std::string("x\ny"));

    CHECK(Json::parse(R"("café 😀")", j));
    CHECK_EQ(j.str(), std::string("café 😀"));
    CHECK(Json::parse(R"("\ud83d")", j));  // lone surrogate
    CHECK_EQ(j.str(), std::string("\xEF\xBF\xBD"));

    std::string err;
    CHECK(!Json::parse("{\"a\":}", j, &err) && !err.empty());
    CHECK(!Json::parse("[1,2", j));
    CHECK(!Json::parse("{} x", j));
    CHECK(!Json::parse(std::string(1000, '['), j));  // depth limit, no stack overflow

    std::string q;
    json_quote(q, std::string("a\"b\\c\x01\xff", 7));
    CHECK_EQ(q, std::string("\"a\\\"b\\\\c\\u0001\xEF\xBF\xBD\""));

    Json o = Json::object();
    o.set("k", 1).set("k", "two").set("n", 0.7);
    CHECK_EQ(o.dump(), std::string(R"({"k":"two","n":0.7})"));
    CHECK(o.erase("k") && !o.has("k"));
}

static void test_util() {
    CHECK_EQ(clean_terminal_output("a\x1b[31mred\x1b[0m\n10%\r50%\r100%\nend\r\n"), std::string("ared\n100%\nend\n"));
    CHECK_EQ(clean_terminal_output("\x1b]0;title\x07ok"), std::string("ok"));
    std::string big;
    for (int i = 0; i < 1000; ++i)
        big += fmt("line %d\n", i);
    std::string ht = head_tail(big, 400);
    CHECK(ht.size() < 500 && starts_with(ht, "line 0\n") && ends_with(ht, "line 999\n"));
    CHECK(ht.find("omitted") != std::string::npos);
    auto a = split_args(R"(load "my model" 'x y' z\ w)");
    CHECK(a.size() == 4 && a[1] == "my model" && a[2] == "x y" && a[3] == "z w");
    CHECK_EQ(utf8_clip("héllo", 2), std::string("h…"));
    CHECK_EQ(utf8_clean("ok\xc3"), std::string("ok\xEF\xBF\xBD"));
    CHECK_EQ(human_ctx(4096), std::string("4k"));
    CHECK_EQ(human_ctx(32768), std::string("32k"));
    CHECK_EQ(human_tokens(1234), std::string("1.2k"));
    CHECK_EQ(plural(1, "line"), std::string("1 line"));
    CHECK_EQ(plural(3, "entry", "entries"), std::string("3 entries"));

    Endpoint ep;
    std::string err;
    CHECK(parse_endpoint("http://10.0.0.5:5555/v1", ep, err) && ep.host == "10.0.0.5" && ep.port == "5555");
    CHECK(parse_endpoint("[::1]:1235", ep, err) && ep.host == "::1" && ep.port == "1235");
    CHECK(parse_endpoint("studio.local", ep, err) && ep.host == "studio.local" && ep.port == "1235");
    CHECK(!parse_endpoint("https://x:1", ep, err));
    CHECK(!parse_endpoint("host:abc", ep, err));
}

static void test_stream_chat() {
    std::vector<std::string> ev = {
        R"({"choices":[{"index":0,"delta":{"role":"assistant","reasoning_content":"hmm "}}]})",
        R"({"choices":[{"index":0,"delta":{"content":"Hel"}}]})",
        R"({"choices":[{"index":0,"delta":{"content":"lo wörld"}}]})",
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"abc","type":"function","function":{"name":"bash","arguments":""}}]}}]})",
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"command\":"}}]}}]})",
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"ls\"}"}}]}}]})",
        R"({"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})",
        R"({"choices":[],"usage":{"prompt_tokens":155,"completion_tokens":23}})",
    };
    Server s = serve({sse_response(ev, 7, 5)});
    LMStudio lm;
    lm.ep = s.ep;
    std::string seen, thought;
    ChatEvents cb;
    cb.content = [&](const std::string& t) { seen += t; };
    cb.reasoning = [&](const std::string& t) { thought += t; };
    ChatReply r = lm.chat(R"({"model":"m","messages":[],"stream":true})", cb);
    std::string req = s.requests();
    CHECK(r.error.empty());
    CHECK_EQ(r.content, std::string("Hello wörld"));
    CHECK_EQ(seen, std::string("Hello wörld"));
    CHECK_EQ(thought, std::string("hmm "));
    CHECK(r.calls.size() == 1);
    if (r.calls.size() == 1) {
        CHECK_EQ(r.calls[0].id, std::string("abc"));
        CHECK_EQ(r.calls[0].name, std::string("bash"));
        CHECK_EQ(r.calls[0].args, std::string(R"({"command":"ls"})"));
    }
    CHECK_EQ(r.finish, std::string("tool_calls"));
    CHECK(r.prompt_tokens == 155 && r.completion_tokens == 23);
    CHECK(req.find("POST /v1/chat/completions HTTP/1.1") != std::string::npos);
    CHECK(req.find("Accept: text/event-stream") != std::string::npos);

    // errors come back as JSON with a non-2xx status
    Server e = serve({json_response(400, R"({"error":{"message":"No models loaded.","type":"x"}})")});
    lm.ep = e.ep;
    r = lm.chat("{}", ChatEvents{});
    e.requests();
    CHECK_EQ(r.error, std::string("No models loaded."));

    // nothing listening
    lm.ep.port = "1";
    r = lm.chat("{}", ChatEvents{});
    CHECK(r.error.find("cannot connect") != std::string::npos);
}

static void test_models() {
    const char* v1 = R"({"models":[
      {"type":"llm","key":"qwen/qwen3-1.7b","display_name":"Qwen3","architecture":"qwen3","quantization":{"name":"Q6_K"},
       "size_bytes":1673178160,"params_string":"1.7B","max_context_length":32768,"format":"gguf",
       "capabilities":{"vision":false,"trained_for_tool_use":true},
       "loaded_instances":[{"id":"qwen/qwen3-1.7b","config":{"context_length":4096}}]},
      {"type":"embedding","key":"nomic","loaded_instances":[],"max_context_length":2048}]})";
    Server s = serve({json_response(200, v1)});
    App app;
    app.lm.ep = s.ep;
    std::string err;
    CHECK(app.refresh_models(err));
    s.requests();
    CHECK(app.lm.api == 1 && app.models.size() == 2);
    if (app.models.size() == 2) {
        const ModelInfo& m = app.models[0];
        CHECK(m.tools && m.max_ctx == 32768 && m.loaded.size() == 1 && m.loaded[0].second == 4096);
        CHECK_EQ(m.quant, std::string("Q6_K"));
    }
    app.pick_default_model();
    CHECK_EQ(app.cfg.model, std::string("qwen/qwen3-1.7b"));
    CHECK(app.ctx_len == 4096);
    CHECK(app.find_model("QWEN3") == &app.models[0]);  // fuzzy, case-insensitive
    CHECK(app.find_model("nothing-like-this", &err) == nullptr && !err.empty());

    // LM Studio < 0.4: v1 is an "unexpected endpoint" (200 OK!), v0 works
    const char* v0 = R"({"data":[{"id":"llama-3.2-3b","type":"llm","arch":"llama","compatibility_type":"mlx",
      "quantization":"4bit","state":"loaded","max_context_length":131072,"loaded_context_length":8192,
      "capabilities":["tool_use"]}]})";
    Server o = serve({json_response(200, R"J({"error":"Unexpected endpoint or method. (GET /api/v1/models)"})J"),
                      json_response(200, v0)});
    app.lm.ep = o.ep;
    CHECK(app.refresh_models(err));
    o.requests();
    CHECK(app.lm.api == 0 && app.models.size() == 1);
    if (!app.models.empty()) CHECK(app.models[0].tools && app.models[0].loaded[0].second == 8192);
}

static void test_tools(const std::string& dir) {
    std::string f = dir + "/a.txt", err;
    write_file(f, "one\n    two  \nthree\nthree\n", err);

    ToolOutput o = run_edit(f, "one\n", "ONE\n");
    CHECK(!o.error);
    o = run_edit(f, "three", "3");
    CHECK(o.error && o.text.find("2 times") != std::string::npos);
    o = run_edit(f, "  two\n", "TWO\n");  // indentation differs: fuzzy line match
    CHECK(!o.error);
    o = run_edit(f, "nope", "x");
    CHECK(o.error && o.text.find("not found") != std::string::npos);
    CHECK(o.text.find("NOT changed") != std::string::npos && o.text.find("three\n") != std::string::npos);
    std::string data;
    read_file(f, data, err);
    CHECK_EQ(data, std::string("ONE\nTWO\nthree\nthree\n"));

    o = run_read(f, 2, 2, 10000);
    CHECK_EQ(o.text, std::string("TWO\nthree\n[lines 2-3 of 4; continue with offset=4]"));
    o = run_read(f, 0, 0, 10);  // byte cap
    CHECK(o.text.find("continue with offset=") != std::string::npos);
    o = run_read(dir, 0, 0, 10000);
    CHECK(o.text.find("a.txt") != std::string::npos);
    o = run_read(dir + "/missing", 0, 0, 1000);
    CHECK(o.error);

    Json args;
    Json::parse("{\"path\":\"" + dir + "/sub/dir/b.txt\",\"content\":\"hi\\n\"}", args);
    o = tool_run("write", args, 1000, 5);
    CHECK(!o.error && is_file(dir + "/sub/dir/b.txt"));
    Json::parse(R"({"cmd":"echo alias"})", args);  // argument alias
    o = tool_run("bash", args, 1000, 5);
    CHECK(o.text.find("alias") != std::string::npos);
    o = tool_run("nope", args, 1000, 5);
    CHECK(o.error);

    o = run_bash("printf 'a\\nb\\n'; exit 3", 1000, 5);
    CHECK_EQ(o.text, std::string("a\nb\n[exit 3]"));
    CHECK(o.error);
    double t0 = now_secs();
    o = run_bash("sleep 10", 1000, 1);
    CHECK(now_secs() - t0 < 5 && o.text.find("timed out") != std::string::npos);
    t0 = now_secs();
    o = run_bash("sleep 30 >/dev/null 2>&1 & echo started", 1000, 20);  // background job must not hang us
    CHECK(now_secs() - t0 < 3 && o.text.find("started") != std::string::npos);
    o = run_bash("seq 1 100000", 2000, 10);
    CHECK(o.text.size() < 2200 && o.text.find("100000") != std::string::npos);
}

static void test_reply_helpers() {
    CHECK_EQ(strip_think("<think>\nplan\n</think>\n\nAnswer"), std::string("Answer"));
    CHECK_EQ(strip_think("plan only\n</think>Answer"), std::string("Answer"));
    CHECK_EQ(strip_think("a<think>x</think>b<think>unfinished"), std::string("ab"));
    std::string c = "Sure.\n<tool_call>\n{\"name\": \"read\", \"arguments\": {\"path\": \"x\"}}\n</tool_call>";
    std::vector<ToolCall> calls;
    extract_text_tool_calls(c, calls);
    CHECK(calls.size() == 1 && calls[0].name == "read" && calls[0].args == R"({"path":"x"})");
    CHECK_EQ(c, std::string("Sure."));
    c = "[TOOL_REQUEST]{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}[END_TOOL_REQUEST]";
    calls.clear();
    extract_text_tool_calls(c, calls);
    CHECK(calls.size() == 1 && calls[0].name == "bash");
    c = "<tool_call>{\"name\":\"rm_rf\",\"arguments\":{}}</tool_call>";  // unknown tools stay text
    calls.clear();
    extract_text_tool_calls(c, calls);
    CHECK(calls.empty());
}

static Json msg(const char* role, const std::string& content) {
    Json m = Json::object();
    m.set("role", role).set("content", content);
    return m;
}

static void fill_history(App& app, int rounds, size_t out_size) {
    app.reset_conversation();
    for (int i = 0; i < rounds; ++i) {
        app.msgs.push_back(msg("user", fmt("step %d", i)));
        Json fn = Json::object();
        Json args = Json::object();
        args.set("path", "f.txt").set("content", std::string(out_size, 'w'));
        fn.set("name", "write").set("arguments", args.dump());
        Json tc = Json::object();
        tc.set("id", fmt("call%05d", i)).set("type", "function").set("function", fn);
        Json am = msg("assistant", "");
        am.set("tool_calls", Json::array().push(tc));
        app.msgs.push_back(am);
        Json tm = msg("tool", std::string(out_size, 'x'));
        tm.set("tool_call_id", fmt("call%05d", i));
        app.msgs.push_back(tm);
        app.msgs.push_back(msg("assistant", "done"));
    }
}

static void test_context_budget() {
    auto answer = [](const char* text) {
        return sse_response({std::string(R"({"choices":[{"index":0,"delta":{"content":")") + text + R"("}}]})",
                             R"({"choices":[],"usage":{"prompt_tokens":100,"completion_tokens":5}})"},
                            64, 4096);
    };
    // 1) trimming old tool output is enough
    Server s = serve({answer("ok")});
    App app;
    app.lm.ep = s.ep;
    app.lm.api = 2;  // skip model management
    app.cfg.model = "m";
    app.ctx_len = 8000;
    fill_history(app, 6, 2000);
    long long before = app.estimate_tokens();
    CHECK(app.run_turn("next") == 0);
    std::string req = s.requests();
    CHECK(app.estimate_tokens() < before);
    CHECK(req.find("trimmed to save context") != std::string::npos);
    CHECK(req.find("Summarize our conversation") == std::string::npos);
    CHECK_EQ(app.msgs.back()["content"].str(), std::string("ok"));
    size_t intact = 0;
    for (auto& m : app.msgs)
        intact += m["role"].str() == "tool" && m["content"].str().size() == 2000;
    CHECK(intact == 3);  // the newest tool results are kept whole

    // 2) still too big: the model summarizes, then answers
    Server c = serve({answer("- summary of work"), answer("fine")});
    app.lm.ep = c.ep;
    app.ctx_len = 2000;
    fill_history(app, 6, 2000);
    CHECK(app.run_turn("next") == 0);
    req = c.requests();
    CHECK(req.find("Summarize our conversation") != std::string::npos);
    CHECK(app.msgs.size() == 5);
    if (app.msgs.size() == 5) {
        CHECK(app.msgs[1]["content"].str().find("- summary of work") != std::string::npos);
        CHECK_EQ(app.msgs[3]["content"].str(), std::string("next"));
        CHECK_EQ(app.msgs[4]["content"].str(), std::string("fine"));
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    char tmpl[] = "/tmp/clil-test-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    test_json();
    test_util();
    test_stream_chat();
    test_models();
    test_tools(dir);
    test_reply_helpers();
    test_context_budget();
    run_attached({"rm", "-rf", dir});
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
