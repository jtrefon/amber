#ifndef AGENT_HTTP_GET_H
#define AGENT_HTTP_GET_H

// A minimal GET for plugin fetches and the skill index.
//
// This is not the chat transport: the LLM request path owns its own client
// (`lib/http_transport.cpp`) because it streams, retries and talks a dialect.
// This is the other kind of call — one small request to an account or a
// registry API — and it exists so callers do not each hand-roll libcurl.
//
// `http_get` keeps the status, the response headers and the body, because a
// registry answer is judged by more than "did bytes arrive": 429's Retry-After
// and 5xx's distinction from a transport failure decide what the UI says.
// `http_get_with_bearer` is the older, narrower shape kept for plugins that
// only need a 2xx body.

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "agent/process.h"

namespace agent {

struct HttpResponse {
    long status = 0; // 0 when no response arrived
    std::string body;
    std::map<std::string, std::string> headers; // keys lowercased
    bool timed_out = false;
};

// Any 1xx-5xx response is returned, including an error status: the caller
// decides what a 401 or a 429 means. `std::nullopt` means the request never
// produced a response (DNS, connect, TLS, or cancellation).
//
// `bearer` empty sends no Authorization header. `cancel` is checked before the
// request and while it is in flight; a cancelled request returns nullopt.
std::optional<HttpResponse> http_get(const std::string& url, const std::string& bearer,
                                     long timeout_s, const CancellationToken& cancel);

// The original narrow contract: body on 2xx, nullopt otherwise. Kept for the
// plugin callers that already depend on it.
std::optional<std::string> http_get_with_bearer(const std::string& url, const std::string& token,
                                                long timeout_s = 20);

// A GET bound to its transport and clock, so a caller can depend on a plain
// function type and a test can answer without curl.
using HttpGet =
    std::function<std::optional<HttpResponse>(const std::string& url, const std::string& bearer,
                                              long timeout_s, const CancellationToken& cancel)>;

} // namespace agent

#endif // AGENT_HTTP_GET_H
