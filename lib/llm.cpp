// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/llm.h"
#include <curl/curl.h>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <chrono>

namespace agent {

namespace {

// Resolve a "{ts}" placeholder in a debug-log path to a process-start stamp.
std::string resolve_debug_path(const std::string& path) {
    using namespace std::chrono;
    static const std::string stamp = std::to_string(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
    std::string r = path;
    size_t pos = r.find("{ts}");
    if (pos != std::string::npos) r.replace(pos, 4, stamp);
    return r;
}

// Append a single labelled record to the debug log. No-op when disabled.
// Binary-safe: writes raw bytes verbatim (useful for raw SSE dumps).
void debug_log(const std::string& path, const std::string& tag,
               const std::string& payload) {
    if (path.empty()) return;
    std::ofstream f(resolve_debug_path(path), std::ios::app | std::ios::binary);
    if (!f) return;
    using namespace std::chrono;
    long long ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    f << "==== " << ms << ' ' << tag << " (" << payload.size() << "B) ====\n"
      << payload << "\n";
}

} // namespace

LLMClient::LLMClient(const Config& cfg) : cfg_(cfg) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

size_t LLMClient::write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* buf = static_cast<std::string*>(user);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

ServerInfo LLMClient::parse_models(const std::string& body) {
    ServerInfo info;
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return info;

    // llama.cpp / OpenAI shape: {"data":[{"id":..,"meta":{"n_ctx":..}}]}
    const json* entry = nullptr;
    if (j.contains("data") && j["data"].is_array() && !j["data"].empty())
        entry = &j["data"][0];
    else if (j.contains("models") && j["models"].is_array() &&
             !j["models"].empty())
        entry = &j["models"][0];
    if (!entry) return info;

    const json& e = *entry;
    if (e.contains("id") && e["id"].is_string())
        info.model = e["id"].get<std::string>();
    else if (e.contains("model") && e["model"].is_string())
        info.model = e["model"].get<std::string>();
    else if (e.contains("name") && e["name"].is_string())
        info.model = e["name"].get<std::string>();

    // Context window lives under meta (llama.cpp): n_ctx (loaded) and
    // n_ctx_train (model max). Accept a top-level n_ctx as a fallback too.
    auto read_int = [](const json& o, const char* k) -> int {
        auto it = o.find(k);
        return (it != o.end() && it->is_number_integer())
                   ? it->get<int>() : 0;
    };
    if (e.contains("meta") && e["meta"].is_object()) {
        const json& m = e["meta"];
        info.context_size = read_int(m, "n_ctx");
        info.context_train = read_int(m, "n_ctx_train");
    }
    if (info.context_size == 0) info.context_size = read_int(e, "n_ctx");
    if (info.context_train == 0) info.context_train = read_int(e, "n_ctx_train");

    info.ok = !info.model.empty() || info.context_size > 0;
    return info;
}

ServerInfo LLMClient::probe_server() const {
    std::string response;
    CURL* c = curl_easy_init();
    if (!c) return {};

    struct curl_slist* headers = nullptr;
    if (!cfg_.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + cfg_.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(c, CURLOPT_URL, cfg_.models_url().c_str());
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);

    CURLcode rc = curl_easy_perform(c);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        debug_log(cfg_.debug_log, "probe-error",
                  std::string(curl_easy_strerror(rc)));
        return {};
    }
    debug_log(cfg_.debug_log, "probe", response);
    return parse_models(response);
}

json LLMClient::build_body(const std::vector<Message>& messages,
                           const std::vector<Tool*>& tools, bool stream) const {
    json body = {
        {"model", cfg_.model},
        {"temperature", cfg_.temperature},
        {"max_tokens", cfg_.max_tokens},
        {"stream", stream},
        {"messages", json::array()}
    };

    // Ask the server to emit a final usage chunk during streaming so we can show
    // context usage and token counts (Qwen/llama.cpp/vLLM honour this).
    if (stream)
        body["stream_options"] = {{"include_usage", true}};

    // Qwen-style thinking control for servers using the model's native jinja
    // chat template (llama.cpp --jinja). The template reads enable_thinking (and
    // an optional thinking_budget) from chat_template_kwargs.
    //   "auto" -> send nothing, defer to the template default.
    if (cfg_.thinking == "on" || cfg_.thinking == "off") {
        bool enable = (cfg_.thinking == "on");
        body["chat_template_kwargs"]["enable_thinking"] = enable;
        if (enable && cfg_.thinking_budget > 0)
            body["chat_template_kwargs"]["thinking_budget"] = cfg_.thinking_budget;
    }

    // Compatibility fallback for OpenAI o-series / vLLM style reasoning servers
    // that use the reasoning_effort field instead of a jinja kwarg.
    if (!cfg_.reasoning_effort.empty() && cfg_.reasoning_effort != "off")
        body["reasoning_effort"] = cfg_.reasoning_effort;

    for (const auto& m : messages) {
        json jm = {{"role", m.role}};
        if (m.role == "assistant" && !m.tool_calls.is_null())
            jm["tool_calls"] = m.tool_calls;
        else if (!m.content.empty())
            jm["content"] = m.content;
        if (m.role == "tool") {
            jm["tool_call_id"] = m.tool_call_id;
            jm["name"] = m.name;
        }
        body["messages"].push_back(jm);
    }

    if (!tools.empty()) {
        json tarr = json::array();
        for (auto* t : tools) {
            tarr.push_back({
                {"type", "function"},
                {"function", {
                    {"name", t->name()},
                    {"description", t->description()},
                    {"parameters", t->parameters_schema()}
                }}
            });
        }
        body["tools"] = tarr;
        body["tool_choice"] = "auto";
    }
    return body;
}

Message LLMClient::chat(const std::vector<Message>& messages,
                        const std::vector<Tool*>& tools,
                        Stats* stats) {
    json body = build_body(messages, tools, false);
    std::string payload = body.dump();
    debug_log(cfg_.debug_log, "request", payload);
    std::string response;

    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!cfg_.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + cfg_.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(c, CURLOPT_URL, cfg_.api_url().c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);

    CURLcode rc = curl_easy_perform(c);
    double ttfb = 0, total = 0;
    curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &ttfb);
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &total);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        debug_log(cfg_.debug_log, "error", std::string(curl_easy_strerror(rc)));
        throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(rc));
    }
    debug_log(cfg_.debug_log, "response", response);

    json resp = json::parse(response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("choices"))
        throw std::runtime_error("malformed LLM response: " + response);

    const json& choice = resp["choices"][0];
    const json& msg = choice["message"];
    Message out;
    out.role = "assistant";
    if (msg.contains("content") && !msg["content"].is_null())
        out.content = msg["content"].get<std::string>();
    for (const char* key : {"reasoning_content", "reasoning"}) {
        if (msg.contains(key) && msg[key].is_string())
            out.reasoning += msg[key].get<std::string>();
    }
    if (msg.contains("tool_calls") && !msg["tool_calls"].is_null())
        out.tool_calls = msg["tool_calls"];

    if (stats) {
        stats->valid = true;
        stats->latency_ms = ttfb * 1000.0;
        if (resp.contains("usage") && resp["usage"].is_object()) {
            const json& u = resp["usage"];
            if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number())
                stats->prompt_tokens = u["prompt_tokens"].get<long>();
            if (u.contains("completion_tokens") && u["completion_tokens"].is_number())
                stats->completion_tokens = u["completion_tokens"].get<long>();
        }
        double gen = total - ttfb;
        if (stats->completion_tokens > 0 && gen > 0.0)
            stats->tps = stats->completion_tokens / gen;
    }
    return out;
}

namespace {

// Mutable state threaded through the streaming write callback so SSE events are
// parsed and dispatched incrementally, as bytes arrive from the network.
struct StreamState {
    std::string buffer;                  // partial SSE line carried across writes
    Message* out = nullptr;              // assembled result
    const std::function<void(const StreamChunk&)>* on_chunk = nullptr;
    std::string debug_path;              // raw-SSE dump target (empty = off)

    // Inline-<think> segmentation: some models put reasoning in the normal
    // content stream wrapped in <think>...</think> rather than a separate
    // reasoning_content field. We split it out on the fly.
    bool in_think = false;
    std::string pending;                 // holds a possible partial tag boundary
    bool finished = false;               // guards against double finalize

    long prompt_tokens = -1;            // from the final usage chunk
    long completion_tokens = -1;
};

// Route a run of normal "content" text through <think> segmentation, appending
// answer text to chunk.delta and reasoning text to chunk.reasoning.
void segment_think(StreamState& st, const std::string& text, StreamChunk& chunk) {
    std::string s = st.pending + text;
    st.pending.clear();
    size_t i = 0;
    while (i < s.size()) {
        if (!st.in_think) {
            size_t open = s.find("<think>", i);
            if (open == std::string::npos) {
                // Keep a short tail back in case a tag straddles the boundary.
                size_t safe = s.size() > 6 ? s.size() - 6 : i;
                if (safe < i) safe = i;
                chunk.delta += s.substr(i, safe - i);
                st.pending = s.substr(safe);
                return;
            }
            chunk.delta += s.substr(i, open - i);
            i = open + 7;
            st.in_think = true;
        } else {
            size_t close = s.find("</think>", i);
            if (close == std::string::npos) {
                size_t safe = s.size() > 7 ? s.size() - 7 : i;
                if (safe < i) safe = i;
                chunk.reasoning += s.substr(i, safe - i);
                st.pending = s.substr(safe);
                return;
            }
            chunk.reasoning += s.substr(i, close - i);
            i = close + 8;
            st.in_think = false;
        }
    }
}

// Flush any bytes held back by segment_think's tag-boundary lookahead and emit
// the terminal chunk. Idempotent so it can be called on [DONE] and again after
// the transfer completes (servers that close without a [DONE] marker).
void finalize_stream(StreamState& st) {
    if (st.finished) return;
    st.finished = true;
    if (!st.pending.empty()) {
        StreamChunk chunk;
        if (st.in_think) chunk.reasoning = st.pending;
        else chunk.delta = st.pending;
        st.pending.clear();
        st.out->content += chunk.delta;
        st.out->reasoning += chunk.reasoning;
        if (st.on_chunk && *st.on_chunk &&
            (!chunk.delta.empty() || !chunk.reasoning.empty()))
            (*st.on_chunk)(chunk);
    }
    if (st.on_chunk && *st.on_chunk) {
        StreamChunk end; end.done = true; (*st.on_chunk)(end);
    }
}

void dispatch_event(StreamState& st, const std::string& data) {
    if (data == "[DONE]") {
        finalize_stream(st);
        return;
    }
    json evt = json::parse(data, nullptr, false);
    if (evt.is_discarded()) return;

    // The include_usage final chunk carries usage and often an empty choices[].
    if (evt.contains("usage") && evt["usage"].is_object()) {
        const json& u = evt["usage"];
        if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number())
            st.prompt_tokens = u["prompt_tokens"].get<long>();
        if (u.contains("completion_tokens") && u["completion_tokens"].is_number())
            st.completion_tokens = u["completion_tokens"].get<long>();
    }

    if (!evt.contains("choices") || evt["choices"].empty())
        return;
    const json& delta = evt["choices"][0].value("delta", json::object());
    StreamChunk chunk;

    // Dedicated reasoning field (OpenAI/vLLM/llama.cpp reasoning models).
    for (const char* key : {"reasoning_content", "reasoning"}) {
        if (delta.contains(key) && delta[key].is_string())
            chunk.reasoning += delta[key].get<std::string>();
    }
    // Normal content, split for inline <think> tags.
    if (delta.contains("content") && delta["content"].is_string())
        segment_think(st, delta["content"].get<std::string>(), chunk);

    if (delta.contains("tool_calls") && !delta["tool_calls"].is_null())
        chunk.tool_calls = delta["tool_calls"];

    st.out->content += chunk.delta;
    st.out->reasoning += chunk.reasoning;

    if (!chunk.tool_calls.is_null()) {
        if (st.out->tool_calls.is_null()) st.out->tool_calls = json::array();
        for (const auto& frag : chunk.tool_calls) {
            int idx = frag.value("index", 0);
            while (static_cast<int>(st.out->tool_calls.size()) <= idx)
                st.out->tool_calls.push_back(json::object());
            json& slot = st.out->tool_calls[idx];
            if (frag.contains("id") && frag["id"].is_string())
                slot["id"] = frag["id"];
            if (frag.contains("type") && frag["type"].is_string())
                slot["type"] = frag["type"];
            json& fn = slot["function"];
            if (fn.is_null()) fn = json::object();
            const json& ffn = frag.value("function", json::object());
            if (ffn.contains("name") && ffn["name"].is_string())
                fn["name"] = ffn["name"];
            if (ffn.contains("arguments") && ffn["arguments"].is_string())
                fn["arguments"] = fn.value("arguments", std::string("")) +
                                  ffn["arguments"].get<std::string>();
        }
    }
    if (st.on_chunk && *st.on_chunk &&
        (!chunk.delta.empty() || !chunk.reasoning.empty() || !chunk.tool_calls.is_null()))
        (*st.on_chunk)(chunk);
}

// libcurl write callback: parse whole SSE lines as they arrive and dispatch
// immediately, buffering any trailing partial line for the next invocation.
size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* st = static_cast<StreamState*>(user);
    if (!st->debug_path.empty())
        debug_log(st->debug_path, "sse-raw",
                  std::string(static_cast<char*>(ptr), size * nmemb));
    st->buffer.append(static_cast<char*>(ptr), size * nmemb);

    size_t nl;
    while ((nl = st->buffer.find('\n')) != std::string::npos) {
        std::string line = st->buffer.substr(0, nl);
        st->buffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("data:", 0) != 0) continue;
        std::string data = line.substr(5);
        size_t p = data.find_first_not_of(" \t");
        if (p != std::string::npos) data = data.substr(p);
        dispatch_event(*st, data);
    }
    return size * nmemb;
}

} // namespace

Message LLMClient::chat_stream(
    const std::vector<Message>& messages,
    const std::vector<Tool*>& tools,
    const std::function<void(const StreamChunk&)>& on_chunk,
    Stats* stats) {
    json body = build_body(messages, tools, true);
    std::string payload = body.dump();
    debug_log(cfg_.debug_log, "request-stream", payload);

    Message out;
    out.role = "assistant";
    StreamState st;
    st.out = &out;
    st.on_chunk = &on_chunk;
    st.debug_path = cfg_.debug_log;

    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    if (!cfg_.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + cfg_.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(c, CURLOPT_URL, cfg_.api_url().c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    // Disable curl's own buffering delays so deltas surface promptly.
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 1024L);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    double ttfb = 0, total = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &ttfb);
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &total);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        debug_log(cfg_.debug_log, "error-stream",
                  std::string(curl_easy_strerror(rc)));
        throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(rc));
    }
    debug_log(cfg_.debug_log, "response-stream",
              "http=" + std::to_string(status) +
              " content=" + out.content +
              "\n---reasoning---\n" + out.reasoning);

    // Safety net: some servers close the stream without a [DONE] marker, which
    // would otherwise leave the boundary-lookahead tail unflushed (truncation).
    finalize_stream(st);

    if (stats) {
        stats->valid = true;
        stats->latency_ms = ttfb * 1000.0;
        stats->prompt_tokens = st.prompt_tokens;
        stats->completion_tokens = st.completion_tokens;
        double gen = total - ttfb;
        if (stats->completion_tokens > 0 && gen > 0.0)
            stats->tps = stats->completion_tokens / gen;
    }
    return out;
}

ServerInfo apply_server_autodetect(Config& cfg) {
    LLMClient client(cfg);
    ServerInfo info = client.probe_server();
    if (!info.ok) return info;
    if (!cfg.model_explicit && !info.model.empty()) {
        cfg.model = info.model;
    }
    if (!cfg.context_explicit && info.context_size > 0) {
        cfg.context_size = info.context_size;
    }
    return info;
}

} // namespace agent
