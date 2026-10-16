#include "lmstudio.hpp"
#include "util.hpp"

#include <cstring>
#include <map>

bool is_missing_endpoint(int status, const Json& body) {
    if (status == 404 && !body["error"]["type"].is_str()) return true;
    return body["error"].is_str() && body["error"].str().find("Unexpected endpoint") != std::string::npos;
}

bool LMStudio::call(const char* method, const std::string& path, const std::string& body, Json& out, std::string& err,
                    int* status, int timeout_ms, const std::function<void()>& tick) {
    HttpOptions opt;
    opt.idle_timeout_ms = timeout_ms;
    opt.tick = tick;
    HttpResult r = http_request(ep, method, path, body, opt);
    if (status) *status = r.status;
    out = Json();
    if (r.status && !Json::parse(r.body, out)) out = Json();
    if (r.ok() && !out["error"].is_null()) {  // LM Studio reports some errors with 200 OK
        err = http_error_text(r);
        return false;
    }
    if (!r.ok()) {
        err = http_error_text(r);
        if (r.status == 401 || r.status == 403) err += " (set LM_API_TOKEN to your LM Studio API token)";
        return false;
    }
    return true;
}

bool LMStudio::list_models(std::vector<ModelInfo>& out, std::string& err) {
    out.clear();
    Json j;
    int st = 0;
    if (call("GET", "/api/v1/models", "", j, err, &st) && j["models"].is_arr()) {
        api = 1;
        for (auto& m : j["models"].items()) {
            ModelInfo mi;
            mi.key = m["key"].str();
            mi.display = m["display_name"].str();
            mi.type = m["type"].str("llm");
            mi.arch = m["architecture"].str();
            mi.quant = m["quantization"]["name"].str();
            mi.format = m["format"].str();
            mi.params = m["params_string"].str();
            mi.size = m["size_bytes"].integer();
            mi.max_ctx = (int)m["max_context_length"].integer();
            mi.tools = m["capabilities"]["trained_for_tool_use"].boolean();
            mi.vision = m["capabilities"]["vision"].boolean();
            for (auto& li : m["loaded_instances"].items())
                mi.loaded.emplace_back(li["id"].str(), (int)li["config"]["context_length"].integer());
            out.push_back(std::move(mi));
        }
        return true;
    }
    if (st == 0 || st == 401 || st == 403) return false;
    // LM Studio before 0.4: the v0 REST API
    if (call("GET", "/api/v0/models", "", j, err, &st) && j["data"].is_arr()) {
        api = 0;
        for (auto& m : j["data"].items()) {
            ModelInfo mi;
            mi.key = m["id"].str();
            mi.type = m["type"].str("llm");
            if (mi.type == "vlm") mi.type = "llm", mi.vision = true;
            if (mi.type == "embeddings") mi.type = "embedding";
            mi.arch = m["arch"].str();
            mi.quant = m["quantization"].str();
            mi.format = m["compatibility_type"].str();
            mi.max_ctx = (int)m["max_context_length"].integer();
            for (auto& c : m["capabilities"].items())
                mi.tools |= c.str() == "tool_use";
            if (m["state"].str() == "loaded")
                mi.loaded.emplace_back(mi.key, (int)m["loaded_context_length"].integer(mi.max_ctx));
            out.push_back(std::move(mi));
        }
        return true;
    }
    if (st == 0) return false;
    // Any other OpenAI-compatible server: names only
    if (call("GET", "/v1/models", "", j, err, &st) && j["data"].is_arr()) {
        api = 2;
        for (auto& m : j["data"].items()) {
            ModelInfo mi;
            mi.key = m["id"].str();
            mi.type = mi.key.find("embed") != std::string::npos ? "embedding" : "llm";
            out.push_back(std::move(mi));
        }
        return true;
    }
    return false;
}

int LMStudio::load(const std::string& model, int ctx, LoadResult& res, std::string& err,
                   const std::function<void()>& tick) {
    Json body = Json::object();
    body.set("model", model).set("echo_load_config", true);
    if (ctx > 0) body.set("context_length", ctx);
    Json j;
    int st = 0;
    // Loading big models takes a while: no idle timeout, ^C cancels the wait.
    if (call("POST", "/api/v1/models/load", body.dump(), j, err, &st, 0, tick)) {
        res.instance = j["instance_id"].str(model.c_str());
        res.secs = j["load_time_seconds"].num();
        res.ctx = (int)j["load_config"]["context_length"].integer(ctx);
        return 1;
    }
    return is_missing_endpoint(st, j) ? 0 : -1;
}

int LMStudio::unload(const std::string& instance, std::string& err) {
    Json body = Json::object();
    body.set("instance_id", instance);
    Json j;
    int st = 0;
    if (call("POST", "/api/v1/models/unload", body.dump(), j, err, &st, 60000)) return 1;
    return is_missing_endpoint(st, j) ? 0 : -1;
}

int LMStudio::download(const std::string& model, Json& res, std::string& err) {
    Json body = Json::object();
    body.set("model", model);
    int st = 0;
    if (call("POST", "/api/v1/models/download", body.dump(), res, err, &st, 120000)) return 1;
    return is_missing_endpoint(st, res) ? 0 : -1;
}

bool LMStudio::download_status(const std::string& job, Json& res, std::string& err) {
    int st = 0;
    return call("GET", "/api/v1/models/download/status/" + job, "", res, err, &st);
}

// ------------------------------------------------------------------ chat

ChatReply LMStudio::chat(const std::string& body, const ChatEvents& ev) {
    ChatReply rep;
    std::string pending;               // bytes after the last complete SSE line
    std::map<long long, size_t> slot;  // tool call stream index -> position in rep.calls
    bool done = false;
    double t0 = now_secs(), t_first = 0;

    auto mark_first = [&]() {
        if (t_first == 0) t_first = now_secs();
    };
    auto on_event = [&](const std::string& data) {
        if (data == "[DONE]") {
            done = true;
            return;
        }
        Json j;
        if (!Json::parse(data, j)) return;
        if (!j["error"].is_null()) {
            const Json& e = j["error"];
            rep.error = e.is_str() ? e.str() : e["message"].str("server error");
            done = true;
            return;
        }
        if (j["usage"].is_obj()) {
            rep.prompt_tokens = j["usage"]["prompt_tokens"].integer(-1);
            rep.completion_tokens = j["usage"]["completion_tokens"].integer(-1);
        }
        const Json& ch = j["choices"][0];
        if (ch["finish_reason"].is_str()) rep.finish = ch["finish_reason"].str();
        const Json& d = ch["delta"].is_obj() ? ch["delta"] : ch["message"];
        for (const char* k : {"reasoning_content", "reasoning"}) {
            const std::string& r = d[k].str();
            if (r.empty()) continue;
            mark_first();
            rep.deltas++;
            rep.reasoning += r;
            if (ev.reasoning) ev.reasoning(r);
        }
        const std::string& c = d["content"].str();
        if (!c.empty()) {
            mark_first();
            rep.deltas++;
            rep.content += c;
            if (ev.content) ev.content(c);
        }
        const Json& tcs = d["tool_calls"];
        for (size_t i = 0; i < tcs.size(); ++i) {
            const Json& tc = tcs[i];
            long long idx = tc["index"].integer((long long)i);
            auto it = slot.find(idx);
            if (it == slot.end()) {
                it = slot.emplace(idx, rep.calls.size()).first;
                rep.calls.emplace_back();
            }
            ToolCall& call = rep.calls[it->second];
            if (call.id.empty()) call.id = tc["id"].str();
            if (call.name.empty()) call.name = tc["function"]["name"].str();
            const Json& a = tc["function"]["arguments"];
            if (a.is_str())
                call.args += a.str();
            else if (a.is_obj())
                call.args = a.dump();
            mark_first();
            rep.deltas++;
            if (ev.tool_progress) ev.tool_progress(call);
        }
    };

    HttpOptions opt;
    opt.idle_timeout_ms = 0;  // first token can take long (prompt processing, JIT load)
    opt.accept = "text/event-stream";
    opt.tick = ev.tick;
    opt.sink = [&](const char* d, size_t n) {
        pending.append(d, n);
        size_t start = 0, nl;
        while (!done && (nl = pending.find('\n', start)) != std::string::npos) {
            size_t end = nl;
            if (end > start && pending[end - 1] == '\r') --end;
            if (end - start >= 5 && pending.compare(start, 5, "data:") == 0) {
                size_t a = start + 5;
                if (a < end && pending[a] == ' ') ++a;
                on_event(pending.substr(a, end - a));
            }
            start = nl + 1;
        }
        pending.erase(0, start);
        return !done;
    };

    HttpResult r = http_request(ep, "POST", "/v1/chat/completions", body, opt);
    if (!done && starts_with(pending, "data:")) on_event(trim(pending.substr(5)));

    rep.interrupted = r.interrupted;
    if (rep.error.empty() && !r.interrupted) {
        if (r.status == 0 || !r.error.empty())
            rep.error = http_error_text(r);
        else if (r.status < 200 || r.status >= 300)
            rep.error = http_error_text(r);
    }
    double t_end = now_secs();
    rep.ttft = t_first > 0 ? t_first - t0 : t_end - t0;
    rep.gen_secs = t_first > 0 ? t_end - t_first : 0;
    for (size_t i = 0; i < rep.calls.size(); ++i)
        if (rep.calls[i].args.empty()) rep.calls[i].args = "{}";
    return rep;
}
