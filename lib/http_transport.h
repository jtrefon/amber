
#ifndef AGENT_HTTP_TRANSPORT_H
#define AGENT_HTTP_TRANSPORT_H

#include "agent/config.h"
#include "agent/llm.h"
#include "agent/sse_parser.h"
#include <curl/curl.h>
#include <memory>
#include <string>

namespace agent {

// RAII wrapper for curl_easy handles. Ensures cleanup on any exit path.
using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
inline CurlPtr make_curl() {
    return CurlPtr(curl_easy_init(), curl_easy_cleanup);
}

// RAII wrapper around a curl_slist of request headers. Owns the list: copying
// would double-free, so copies are deleted and moves are defaulted.
struct HeaderList {
    curl_slist* list = nullptr;
    ~HeaderList();
    HeaderList() = default;
    HeaderList(const HeaderList&) = delete;
    HeaderList& operator=(const HeaderList&) = delete;
    HeaderList(HeaderList&& other) noexcept : list(other.list) {
        other.list = nullptr;
    }
    HeaderList& operator=(HeaderList&& other) noexcept {
        if (this != &other) {
            if (list) curl_slist_free_all(list);
            list = other.list;
            other.list = nullptr;
        }
        return *this;
    }
    void add(const std::string& h) {
        list = curl_slist_append(list, h.c_str());
    }
};

void apply_auth(HeaderList& h, const Config& cfg);

// POST `payload` to the chat endpoint; return the raw response body (or throw on
// transport error). `accept_sse` adds the text/event-stream Accept header.
// `ttfb`/`total` receive transfer timings in seconds when non-null.
std::string post_completion(Config& cfg, const std::string& payload,
                            bool accept_sse, double* ttfb, double* total);

// Run a streaming completion: POST `payload`, feed SSE bytes to `parser`, and
// finalize. Fills `stats` (timings + token counts). Throws on transport error.
// `cfg` is non-const so a 400 overflow rejection can teach the runtime
// context window (the host pulls it via LLMClient::learned_context_size()).
void stream_completion(Config& cfg, const std::string& payload,
                       StreamParser& parser, Stats* stats, long& status_out);

// Parse a buffered /chat/completions JSON body into a Message. Degrades
// gracefully on malformed/error responses: the message carries the raw body as
// text so the agent loop can feed it back to the model (never throws).
Message message_from_completion(const std::string& response);

// Build the human-readable HTTP error message. When the body carries a known
// server-side failure mode, an actionable hint is appended (e.g. llama.cpp
// chat-template parser generation failures, which are a server/model problem,
// not a request problem).
std::string describe_http_error(long http_code, const std::string& body);

// True when a non-2xx response is a transient upstream failure rather than a
// request rejection. Gateways (kilocode's OpenAI-compatible router among
// them) surface an overloaded/crashed upstream as HTTP 400 whose body is an
// empty SSE stream (at most comments / a bare [DONE]) — retrying that shape
// rides through the blip, while a genuine schema-rejection 400 (JSON error
// body) stays non-retryable.
bool is_retryable_http_error(long http_code, const std::string& body);

// Fill `stats` from a buffered response body and its transfer timings (seconds).
// Mirrors the telemetry that stream_completion() produces for the streamed path.
void fill_buffered_stats(Stats& stats, const std::string& response, double ttfb,
                         double total);

} // namespace agent

#endif // AGENT_HTTP_TRANSPORT_H
