// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "http_transport.h"
#include "agent/agent_helpers.h"
#include "agent/debug_log.h"
#include "agent/process.h"

#include <curl/curl.h>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace agent {

namespace {

// libcurl progress callback — aborts the transfer when the tool-cancel
// flag is set, so Esc / /stop interrupts a blocking LLM call promptly.
// The `clientp` argument is a `const CancellationToken*` set via
// CURLOPT_XFERINFODATA. A null or uncontested token is a no-op.
int cancel_check_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    if (!clientp) return 0;
    const auto* token = static_cast<const CancellationToken*>(clientp);
    return token->is_requested() ? 1 : 0;
}

// libcurl write callback shim: variadic_setopt cannot convert a lambda to a
// function pointer, so we need a named function. Forwards to StreamParser.
size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    return static_cast<StreamParser*>(user)->on_write(ptr, size, nmemb);
}

long read_usage_token(const json& usage, const char* key) {
    auto it = usage.find(key);
    return (it != usage.end() && it->is_number()) ? it->get<long>() : -1;
}

void apply_tps(Stats& stats, double ttfb, double total) {
    double gen = total - ttfb;
    if (stats.completion_tokens > 0 && gen > 0.0)
        stats.tps = stats.completion_tokens / gen;
}

} // namespace

HeaderList::~HeaderList() {
    if (list) curl_slist_free_all(list);
}

void apply_auth(HeaderList& h, const Config& cfg) {
    if (!cfg.api_key.empty())
        h.add("Authorization: Bearer " + cfg.api_key);
}

// Extract a string field defensively: returns d if missing, null, or not a
// string (so a malformed model response never throws and aborts the turn).
std::string str_or_raw(const json& j, const char* key, const std::string& d) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return d;
    if (it->is_string()) return it->get<std::string>();
    // Non-string content: keep it as JSON text rather than throwing, so the
    // pipeline can feed it back to the model instead of crashing.
    return it->dump();
}

// Parse a buffered /chat/completions JSON body into a Message. Degrades
// gracefully on malformed/error responses: a parse failure or missing choices
// yields an assistant message carrying the raw body as text, which the agent
// loop feeds back to the model so it can recover instead of aborting the turn.
Message message_from_completion(const std::string& response) {
    json resp = json::parse(response, nullptr, false);
    Message out;
    out.role = "assistant";
    if (resp.is_discarded() || !resp.contains("choices") ||
        !resp["choices"].is_array() || resp["choices"].empty()) {
        out.content =
            "[error: malformed LLM response, raw body follows]\n" + response;
        return out;
    }
    const json& msg = resp["choices"][0].value("message", json::object());
    out.content = strip_think(str_or_raw(msg, "content", ""));
    for (const char* key : {"reasoning_content", "reasoning"})
        out.reasoning += str_or_raw(msg, key, "");
    if (msg.contains("tool_calls") && !msg["tool_calls"].is_null()) {
        out.tool_calls = msg["tool_calls"];
        // Discard tool calls with non-JSON arguments — they poison history.
        bool valid = true;
        for (const auto& tc : out.tool_calls) {
            auto fn = tc.value("function", json::object());
            std::string raw = fn.value("arguments", "");
            if (!raw.empty()) {
                auto parsed = json::parse(raw, nullptr, false);
                if (parsed.is_discarded()) { valid = false; break; }
            }
        }
        if (!valid) out.tool_calls = json::value_t::null;
    }
    return out;
}

// Fill `stats` from a buffered response body and its transfer timings.
void fill_buffered_stats(Stats& stats, const std::string& response, double ttfb,
                         double total) {
    stats.valid = true;
    stats.latency_ms = ttfb * 1000.0;
    json resp = json::parse(response, nullptr, false);
    if (resp.contains("usage") && resp["usage"].is_object()) {
        const json& u = resp["usage"];
        stats.prompt_tokens = read_usage_token(u, "prompt_tokens");
        stats.completion_tokens = read_usage_token(u, "completion_tokens");
    }
    apply_tps(stats, ttfb, total);
}

namespace {

// Shared curl request execution: sets up headers, URL, POST body, write
// callback, timeout, and cancel wiring, then performs the request and
// collects timing + status. Throws on transport or HTTP error.
void curl_exec(const Config& cfg, const std::string& payload,
               bool accept_sse, long timeout_s,
               curl_write_callback write_fn, void* write_data,
               long& http_code, double& ttfb, double& total,
               const char* debug_tag) {
    auto c = make_curl();
    if (!c) throw std::runtime_error("curl_easy_init failed");
    HeaderList headers;
    headers.add("Content-Type: application/json");
    if (accept_sse) headers.add("Accept: text/event-stream");
    apply_auth(headers, cfg);

    curl_easy_setopt(c.get(), CURLOPT_URL, cfg.api_url().c_str());
    curl_easy_setopt(c.get(), CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(c.get(), CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(c.get(), CURLOPT_WRITEFUNCTION, write_fn);
    curl_easy_setopt(c.get(), CURLOPT_WRITEDATA, write_data);
    curl_easy_setopt(c.get(), CURLOPT_TIMEOUT, timeout_s);
    // Small buffer for SSE streaming — surface reasoning/delta chunks
    // promptly instead of buffering 16KB+ at the transport layer.
    if (accept_sse) {
        curl_easy_setopt(c.get(), CURLOPT_BUFFERSIZE, 1024L);
        // Abort if no data arrives for 60s (detects hung server quickly
        // without interfering with legitimate long generations).
        curl_easy_setopt(c.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    }
    curl_easy_setopt(c.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c.get(), CURLOPT_XFERINFOFUNCTION, cancel_check_cb);
    curl_easy_setopt(c.get(), CURLOPT_XFERINFODATA, &cfg.cancel_token);

    CURLcode rc = curl_easy_perform(c.get());
    curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(c.get(), CURLINFO_STARTTRANSFER_TIME, &ttfb);
    curl_easy_getinfo(c.get(), CURLINFO_TOTAL_TIME, &total);
    if (rc != CURLE_OK) {
        debug_log(cfg.debug_log, debug_tag, std::string(curl_easy_strerror(rc)));
        throw std::runtime_error(std::string("curl error: ") +
                                 curl_easy_strerror(rc));
    }
}

} // namespace

// POST `payload` to the chat endpoint and return the raw response body, setting
// up auth/JSON headers and throwing on any transport error. `accept_sse` adds
// the text/event-stream Accept header for streaming requests. When non-null,
// `ttfb`/`total` receive transfer timings in seconds.
std::string post_completion(const Config& cfg, const std::string& payload,
                            bool accept_sse, double* ttfb, double* total) {
    std::string response;
    long http_code = 0;
    double t0 = 0, t1 = 0;
    curl_exec(cfg, payload, accept_sse, accept_sse ? 300L : 300L,
              LLMClient::write_cb, &response,
              http_code, t0, t1, "error");
    if (ttfb) *ttfb = t0;
    if (total) *total = t1;
    if (http_code < 200 || http_code >= 300) {
        std::string snippet = response.substr(0, 200);
        throw std::runtime_error("HTTP " + std::to_string(http_code) +
                                 " from LLM server: " + snippet);
    }
    return response;
}

// Run a streaming completion: POST `payload`, feed SSE bytes to `parser`, and
// finalize. Fills `stats` (timings + token counts). Throws on transport error.
void stream_completion(const Config& cfg, const std::string& payload,
                       StreamParser& parser, Stats* stats, long& status_out) {
    double ttfb = 0, total = 0;
    curl_exec(cfg, payload, true, 300L,
              stream_write_cb, &parser,
              status_out, ttfb, total, "error-stream");
    if (status_out < 200 || status_out >= 300) {
        std::string detail = parser.raw_body_.substr(0, 400);
        throw std::runtime_error("HTTP " + std::to_string(status_out) +
                                 " from LLM server: " + detail);
    }
    parser.finalize();
    if (stats) {
        stats->valid = true;
        stats->latency_ms = ttfb * 1000.0;
        stats->prompt_tokens = parser.prompt_tokens();
        stats->completion_tokens = parser.completion_tokens();
        apply_tps(*stats, ttfb, total);
    }
}

} // namespace agent
