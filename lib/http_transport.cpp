
#include "http_transport.h"
#include "agent/debug_log.h"
#include "agent/dialect.h"
#include "agent/process.h"

#include <curl/curl.h>

namespace {
// libcurl write callback: accumulate the response body into a std::string.
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* buf = static_cast<std::string*>(user);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace
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
// function pointer, so we need a named function. Forwards to the StreamDecoder.
size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    return static_cast<StreamDecoder*>(user)->on_write(ptr, size, nmemb);
}

void apply_tps(Stats& stats, double ttfb, double total) {
    double gen = total - ttfb;
    if (stats.completion_tokens > 0 && gen > 0.0)
        stats.tps = stats.completion_tokens / gen;
}

// Shared curl request execution: sets up the dialect's URL and auth headers,
// the POST body, write callback, timeout, and cancel wiring, then performs the
// request and collects timing + status. Throws on transport error.
void curl_exec(const Config& cfg, const Dialect& dialect,
               const std::string& payload, bool accept_sse, long timeout_s,
               curl_write_callback write_fn, void* write_data,
               long& http_code, double& ttfb, double& total,
               const char* debug_tag) {
    auto c = make_curl();
    if (!c) throw std::runtime_error("curl_easy_init failed");
    HeaderList headers;
    headers.add("Content-Type: application/json");
    if (accept_sse) headers.add("Accept: text/event-stream");
    for (const auto& h : dialect.auth_headers(cfg)) headers.add(h);

    const std::string url = dialect.chat_url(cfg);
    curl_easy_setopt(c.get(), CURLOPT_URL, url.c_str());
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
        if (cfg.cancel_token.is_requested())
            throw CancelledError("request cancelled by user");
        throw std::runtime_error(std::string("curl error: ") +
                                 curl_easy_strerror(rc));
    }
}

} // namespace

HeaderList::~HeaderList() {
    if (list) curl_slist_free_all(list);
}

std::string describe_http_error(long http_code, const std::string& body) {
    std::string msg = "HTTP " + std::to_string(http_code) +
                      " from LLM server: " + body.substr(0, 200);
    // llama.cpp "automatic parser generation" failures mean the loaded
    // model's chat template cannot be auto-parsed for tool calling — a
    // server/model problem, not a request problem. Say so instead of
    // leaving the user to decode the raw nlohmann error.
    if (body.find("Unable to generate parser for this template") !=
        std::string::npos)
        msg += "  (server chat-template parser failure: reload the model "
               "on the server or check its template)";
    return msg;
}

// POST `payload` to the dialect's chat endpoint and return the raw response
// body, setting up auth/JSON headers and throwing on any transport error.
// `accept_sse` adds the text/event-stream Accept header for streaming
// requests. When non-null, `ttfb`/`total` receive transfer timings in seconds.
std::string post_completion(Config& cfg, const Dialect& dialect,
                            const std::string& payload, bool accept_sse,
                            double* ttfb, double* total) {
    std::string response;
    long http_code = 0;
    double t0 = 0, t1 = 0;
    curl_exec(cfg, dialect, payload, accept_sse, 300L,
              write_cb, &response,
              http_code, t0, t1, "error");
    if (ttfb) *ttfb = t0;
    if (total) *total = t1;
    if (http_code < 200 || http_code >= 300) {
        // Try to learn context_size from HTTP 400 overflow errors. The
        // rejection is the runtime truth — it clamps even an explicitly
        // configured window (e.g. a model whose trained n_ctx exceeds the
        // server's actual --ctx-size). The host pulls the learned value via
        // LLMClient::learned_context_size().
        if (http_code == 400) {
            int learned = dialect.context_overflow_hint(response);
            if (learned > 0) {
                cfg.context_size = learned;
                cfg.context_explicit = true;
            }
        }
        bool retryable = dialect.is_retryable(http_code, response);
        throw ApiError(http_code, retryable, describe_http_error(http_code, response));
    }
    return response;
}

// Run a streaming completion: POST `payload`, feed response bytes to
// `decoder`, and finalize. Fills `stats` (timings + token counts). Throws on
// transport error.
void stream_completion(Config& cfg, const Dialect& dialect,
                       const std::string& payload, StreamDecoder& decoder,
                       Stats* stats, long& status_out) {
    double ttfb = 0, total = 0;
    curl_exec(cfg, dialect, payload, true, 300L,
              stream_write_cb, &decoder,
              status_out, ttfb, total, "error-stream");
    if (status_out < 200 || status_out >= 300) {
        // Same overflow learning as the buffered path: the rejection teaches
        // the runtime window regardless of any configured value.
        if (status_out == 400) {
            int learned = dialect.context_overflow_hint(decoder.raw_body());
            if (learned > 0) {
                cfg.context_size = learned;
                cfg.context_explicit = true;
            }
        }
        bool retryable = dialect.is_retryable(status_out, decoder.raw_body());
        throw ApiError(status_out, retryable,
                       describe_http_error(status_out, decoder.raw_body()));
    }
    decoder.finalize();
    if (stats) {
        stats->valid = true;
        stats->latency_ms = ttfb * 1000.0;
        stats->prompt_tokens = decoder.prompt_tokens();
        stats->completion_tokens = decoder.completion_tokens();
        apply_tps(*stats, ttfb, total);
    }
}

// Fill `stats` from a buffered response body and its transfer timings
// (seconds), mapping the dialect's token usage. Mirrors the telemetry that
// stream_completion() produces for the streamed path.
void fill_buffered_stats(Stats& stats, const Dialect& dialect,
                         const std::string& response, double ttfb,
                         double total) {
    stats.valid = true;
    stats.latency_ms = ttfb * 1000.0;
    const TokenUsage usage = dialect.parse_usage(response);
    stats.prompt_tokens = usage.prompt;
    stats.completion_tokens = usage.completion;
    apply_tps(stats, ttfb, total);
}

} // namespace agent
