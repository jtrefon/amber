
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

// RAII wrapper around a curl_slist of request headers.
struct HeaderList {
    curl_slist* list = nullptr;
    ~HeaderList();
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
void stream_completion(const Config& cfg, const std::string& payload,
                       StreamParser& parser, Stats* stats, long& status_out);

// Parse a buffered /chat/completions JSON body into a Message, throwing on a
// malformed or error response.
Message message_from_completion(const std::string& response);

// Fill `stats` from a buffered response body and its transfer timings (seconds).
// Mirrors the telemetry that stream_completion() produces for the streamed path.
void fill_buffered_stats(Stats& stats, const std::string& response, double ttfb,
                         double total);

} // namespace agent

#endif // AGENT_HTTP_TRANSPORT_H
