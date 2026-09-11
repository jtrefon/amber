#ifndef AGENT_HTTP_GET_H
#define AGENT_HTTP_GET_H

// A minimal authenticated GET for plugin fetches (a wallet, a status probe).
//
// This is not the chat transport: the LLM request path owns its own client
// (`lib/http_transport.cpp`) because it streams, retries and talks a dialect.
// This is the other kind of call — one small request to a provider's own
// account API — and it exists so plugins do not each hand-roll libcurl.
//
// Returns the response body, or nullopt on any transport failure, non-2xx
// status, or when the token is empty. Never throws: a failed fetch is a normal
// outcome the caller reports, not an exception.

#include <optional>
#include <string>

namespace agent {

std::optional<std::string> http_get_with_bearer(const std::string& url, const std::string& token,
                                                long timeout_s = 20);

} // namespace agent

#endif // AGENT_HTTP_GET_H
