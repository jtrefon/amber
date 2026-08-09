
#include "agent/llm.h"
#include "agent/debug_log.h"
#include "http_transport.h"
#include "agent/model_probe.h"
#include "agent/request_builder.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <utility>

namespace agent {

HttpLLMClient::HttpLLMClient(Config cfg) : cfg_(std::move(cfg)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ServerInfo LLMClient::parse_models(const std::string& body,
                                   const std::string& preferred_model) {
    return agent::parse_models(body, preferred_model);
}

ServerInfo HttpLLMClient::probe_server() const {
    return agent::probe_server(cfg_);
}

Message HttpLLMClient::chat(const std::vector<Message>& messages,
                        const std::vector<std::shared_ptr<Tool>>& tools, Stats* stats) {
    json body = build_chat_body(cfg_, messages, tools, false);
    // Tool/model text can contain invalid UTF-8 (e.g. binary from grep);
    // nlohmann throws type_error.316 on dump() unless we replace bad bytes.
    std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);
    debug_log(cfg_.debug_log, "request", payload);

    double ttfb = 0, total = 0;
    const int ctx_before = cfg_.context_size;
    std::string response = post_completion(cfg_, payload, false, &ttfb, &total);
    if (cfg_.context_size != ctx_before) learned_ = cfg_.context_size;
    debug_log(cfg_.debug_log, "response", response);

    Message out = message_from_completion(response);
    if (stats) fill_buffered_stats(*stats, response, ttfb, total);
    return out;
}

Message HttpLLMClient::chat_stream(const std::vector<Message>& messages,
                               const std::vector<std::shared_ptr<Tool>>& tools,
                               const std::function<void(const StreamChunk&)>& on_chunk,
                               Stats* stats) {
    json body = build_chat_body(cfg_, messages, tools, true);
    std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);
    debug_log(cfg_.debug_log, "request-stream", payload);

    Message out;
    out.role = "assistant";
    StreamParser parser(out, on_chunk, cfg_.debug_log);

    long status = 0;
    const int ctx_before = cfg_.context_size;
    stream_completion(cfg_, payload, parser, stats, status);
    if (cfg_.context_size != ctx_before) learned_ = cfg_.context_size;
    debug_log(cfg_.debug_log, "response-stream",
              "http=" + std::to_string(status) +
                  " content=" + out.content +
                  "\n---reasoning---\n" + out.reasoning);

    // Validate tool call arguments: if any tool call has non-JSON arguments,
    // discard ALL tool calls and keep only text.  Malformed tool calls in
    // history poison subsequent requests (the server rejects them).
    if (!out.tool_calls.is_null() && out.tool_calls.is_array()) {
        bool valid = true;
        for (const auto& tc : out.tool_calls) {
            auto fn = tc.value("function", json::object());
            std::string raw = fn.value("arguments", "");
            if (!raw.empty()) {
                auto parsed = json::parse(raw, nullptr, false);
                if (parsed.is_discarded()) { valid = false; break; }
            }
        }
        if (!valid) {
            debug_log(cfg_.debug_log, "response-stream",
                      "discarding malformed tool calls");
            out.tool_calls = json::value_t::null;
        }
    }

    return out;
}

} // namespace agent
