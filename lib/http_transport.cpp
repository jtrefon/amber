
#include "http_transport.h"
#include "agent/debug_log.h"
#include "agent/dialect.h"
#include "agent/process.h"
#include "agent/rate_limit.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
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

long parse_retry_after_ms(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    const auto end = value.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos || end == std::string::npos || end < begin)
        return 0;
    const std::string text = value.substr(begin, end - begin + 1);
    errno = 0;
    char* stop = nullptr;
    const long seconds = std::strtol(text.c_str(), &stop, 10);
    if (errno != 0 || stop == text.c_str() || *stop != '\0' || seconds < 0)
        return 0; // an HTTP-date or garbage: the policy's exponential fallback
    return std::min(seconds * 1000L, 300000L);
}

RequestPacer& RequestPacer::global() {
    static RequestPacer pacer;
    return pacer;
}

bool RequestPacer::before_request(const CancellationToken& token) {
    std::unique_lock lock(mutex_);
    for (;;) {
        if (token.is_requested())
            return false;
        const auto now = std::chrono::steady_clock::now();
        const auto until = std::max(last_start_ + policy_.interval, cooldown_until_);
        if (now >= until) {
            last_start_ = now;
            return true;
        }
        cv_.wait_for(lock, std::min<std::chrono::steady_clock::duration>(
                               until - now, std::chrono::milliseconds(200)));
    }
}

void RequestPacer::on_rate_limit(std::chrono::milliseconds retry_after) {
    std::lock_guard lock(mutex_);
    policy_.note_rate_limit(retry_after);
    cooldown_until_ = std::chrono::steady_clock::now() + policy_.cooldown;
    cv_.notify_all();
}

void RequestPacer::on_success() {
    std::lock_guard lock(mutex_);
    policy_.note_success();
    cv_.notify_all();
}

RateLimitPolicy RequestPacer::policy() const {
    std::lock_guard lock(mutex_);
    return policy_;
}

namespace {

// Retry-After capture from the response headers; numeric seconds only.
struct RetryCapture {
    long retry_after_ms = 0;
};

size_t retry_header_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* capture = static_cast<RetryCapture*>(user);
    const std::string line(ptr, size * nmemb);
    const auto colon = line.find(':');
    if (colon == std::string::npos)
        return size * nmemb;
    std::string name = line.substr(0, colon);
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "retry-after")
        capture->retry_after_ms = parse_retry_after_ms(line.substr(colon + 1));
    return size * nmemb;
}

// libcurl progress callback — aborts the transfer when the tool-cancel
// flag is set, so Esc / /stop interrupts a blocking LLM call promptly.
// The `clientp` argument is a `const CancellationToken*` set via
// CURLOPT_XFERINFODATA. A null or uncontested token is a no-op.
int cancel_check_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    if (!clientp)
        return 0;
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

// Result of one request, including the rate-limit signal the pacer needs.
struct CurlOutcome {
    long status = 0;
    double ttfb = 0;
    double total = 0;
    long retry_after_ms = 0;
};

void configure_target(CURL* c, const Config& cfg, const Dialect& dialect,
                      const std::string& payload) {
    const std::string url = dialect.chat_url(cfg);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
}

void configure_write(CURL* c, curl_write_callback fn, void* data) {
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fn);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, data);
}

void configure_headers(CURL* c, const Config& cfg, const Dialect& dialect, bool accept_sse,
                       HeaderList& headers, RetryCapture& retry) {
    headers.add("Content-Type: application/json");
    if (accept_sse)
        headers.add("Accept: text/event-stream");
    for (const auto& h : dialect.auth_headers(cfg))
        headers.add(h);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, retry_header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &retry);
}

void configure_cancel(CURL* c, const CancellationToken& token) {
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, cancel_check_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &token);
}

// Surface reasoning/delta chunks promptly instead of buffering 16 KB+, and
// abort if no data arrives for 60 s (a hung server, not a long generation).
void configure_stream_buffers(CURL* c) {
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
}

// Every caller shares one adaptive gate: a 429 sets a cooldown and widens the
// spacing; a success lets it decay. The cooldown is what stops a retrying agent
// or a sibling subagent from re-firing immediately.
void note_response(const CurlOutcome& out) {
    if (out.status == 429)
        RequestPacer::global().on_rate_limit(std::chrono::milliseconds(out.retry_after_ms));
    else if (out.status >= 200 && out.status < 300)
        RequestPacer::global().on_success();
}

// Shared curl request execution: sets up the dialect's URL and auth headers,
// the POST body, write callback, timeout, and cancel wiring, then performs the
// request and collects timing + status. Throws on transport error.
void curl_exec(const Config& cfg, const Dialect& dialect, const std::string& payload,
               bool accept_sse, curl_write_callback write_fn, void* write_data, CurlOutcome& out,
               const char* debug_tag) {
    if (!RequestPacer::global().before_request(cfg.cancel_token))
        throw CancelledError("cancelled while waiting for the provider rate limit");
    auto c = make_curl();
    if (!c)
        throw std::runtime_error("curl_easy_init failed");
    RetryCapture retry;
    HeaderList headers;
    configure_target(c.get(), cfg, dialect, payload);
    configure_write(c.get(), write_fn, write_data);
    configure_headers(c.get(), cfg, dialect, accept_sse, headers, retry);
    curl_easy_setopt(c.get(), CURLOPT_TIMEOUT, 300L);
    if (accept_sse)
        configure_stream_buffers(c.get());
    configure_cancel(c.get(), cfg.cancel_token);

    const CURLcode rc = curl_easy_perform(c.get());
    curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &out.status);
    curl_easy_getinfo(c.get(), CURLINFO_STARTTRANSFER_TIME, &out.ttfb);
    curl_easy_getinfo(c.get(), CURLINFO_TOTAL_TIME, &out.total);
    out.retry_after_ms = retry.retry_after_ms;
    note_response(out);
    if (rc != CURLE_OK) {
        debug_log(cfg.debug_dump_path(), debug_tag, std::string(curl_easy_strerror(rc)));
        if (cfg.cancel_token.is_requested())
            throw CancelledError("request cancelled by user");
        throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(rc));
    }
}

} // namespace

HeaderList::~HeaderList() {
    if (list)
        curl_slist_free_all(list);
}

std::string describe_http_error(long http_code, const std::string& body) {
    std::string msg =
        "HTTP " + std::to_string(http_code) + " from LLM server: " + body.substr(0, 200);
    // llama.cpp "automatic parser generation" failures mean the loaded
    // model's chat template cannot be auto-parsed for tool calling — a
    // server/model problem, not a request problem. Say so instead of
    // leaving the user to decode the raw nlohmann error.
    if (body.find("Unable to generate parser for this template") != std::string::npos)
        msg += "  (server chat-template parser failure: reload the model "
               "on the server or check its template)";
    return msg;
}

// POST `payload` to the dialect's chat endpoint and return the raw response
// body, setting up auth/JSON headers and throwing on any transport error.
// `accept_sse` adds the text/event-stream Accept header for streaming
// requests. When non-null, `ttfb`/`total` receive transfer timings in seconds.
std::string post_completion(Config& cfg, const Dialect& dialect, const std::string& payload,
                            bool accept_sse, double* ttfb, double* total) {
    std::string response;
    CurlOutcome out;
    curl_exec(cfg, dialect, payload, accept_sse, write_cb, &response, out, "error");
    if (ttfb)
        *ttfb = out.ttfb;
    if (total)
        *total = out.total;
    if (out.status < 200 || out.status >= 300) {
        // Try to learn context_size from HTTP 400 overflow errors. The
        // rejection is the runtime truth — it clamps even an explicitly
        // configured window (e.g. a model whose trained n_ctx exceeds the
        // server's actual --ctx-size). The host pulls the learned value via
        // LLMClient::learned_context_size().
        if (out.status == 400) {
            int learned = dialect.context_overflow_hint(response);
            if (learned > 0) {
                cfg.context_size = learned;
                cfg.context_explicit = true;
            }
        }
        bool retryable = dialect.is_retryable(out.status, response);
        throw ApiError(out.status, retryable, describe_http_error(out.status, response),
                       out.retry_after_ms);
    }
    return response;
}

// Run a streaming completion: POST `payload`, feed response bytes to
// `decoder`, and finalize. Fills `stats` (timings + token counts). Throws on
// transport error.
void stream_completion(Config& cfg, const Dialect& dialect, const std::string& payload,
                       StreamDecoder& decoder, Stats* stats, long& status_out) {
    CurlOutcome out;
    curl_exec(cfg, dialect, payload, true, stream_write_cb, &decoder, out, "error-stream");
    status_out = out.status;
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
        throw ApiError(status_out, retryable, describe_http_error(status_out, decoder.raw_body()),
                       out.retry_after_ms);
    }
    decoder.finalize();
    if (stats) {
        stats->valid = true;
        stats->latency_ms = out.ttfb * 1000.0;
        stats->prompt_tokens = decoder.prompt_tokens();
        stats->completion_tokens = decoder.completion_tokens();
        apply_tps(*stats, out.ttfb, out.total);
    }
}

// Fill `stats` from a buffered response body and its transfer timings
// (seconds), mapping the dialect's token usage. Mirrors the telemetry that
// stream_completion() produces for the streamed path.
void fill_buffered_stats(Stats& stats, const Dialect& dialect, const std::string& response,
                         double ttfb, double total) {
    stats.valid = true;
    stats.latency_ms = ttfb * 1000.0;
    const TokenUsage usage = dialect.parse_usage(response);
    stats.prompt_tokens = usage.prompt;
    stats.completion_tokens = usage.completion;
    apply_tps(stats, ttfb, total);
}

} // namespace agent
