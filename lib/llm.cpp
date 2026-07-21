// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/llm.h"
#include "agent/debug_log.h"
#include "http_transport.h"
#include "agent/model_probe.h"
#include "agent/request_builder.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <utility>

namespace agent {

LLMClient::LLMClient(Config  cfg) : cfg_(std::move(cfg)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

size_t LLMClient::write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* buf = static_cast<std::string*>(user);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

ServerInfo LLMClient::parse_models(const std::string& body) {
    return agent::parse_models(body);
}

ServerInfo LLMClient::probe_server() const {
    return agent::probe_server(cfg_);
}

Message LLMClient::chat(const std::vector<Message>& messages,
                        const std::vector<Tool*>& tools, Stats* stats) {
    json body = build_chat_body(cfg_, messages, tools, false);
    // Tool/model text can contain invalid UTF-8 (e.g. binary from grep);
    // nlohmann throws type_error.316 on dump() unless we replace bad bytes.
    std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);
    debug_log(cfg_.debug_log, "request", payload);

    double ttfb = 0, total = 0;
    std::string response = post_completion(cfg_, payload, false, &ttfb, &total);
    debug_log(cfg_.debug_log, "response", response);

    Message out = message_from_completion(response);
    if (stats) fill_buffered_stats(*stats, response, ttfb, total);
    return out;
}

Message LLMClient::chat_stream(const std::vector<Message>& messages,
                               const std::vector<Tool*>& tools,
                               const std::function<void(const StreamChunk&)>& on_chunk,
                               Stats* stats) {
    json body = build_chat_body(cfg_, messages, tools, true);
    std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);
    debug_log(cfg_.debug_log, "request-stream", payload);

    Message out;
    out.role = "assistant";
    StreamParser parser(out, on_chunk, cfg_.debug_log);

    long status = 0;
    stream_completion(cfg_, payload, parser, stats, status);
    debug_log(cfg_.debug_log, "response-stream",
              "http=" + std::to_string(status) +
                  " content=" + out.content +
                  "\n---reasoning---\n" + out.reasoning);
    return out;
}

} // namespace agent
