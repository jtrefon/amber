#ifndef AGENT_PLUGINS_WEB_CLIENT_H
#define AGENT_PLUGINS_WEB_CLIENT_H

#include "web_parse.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent::plugins::web {

struct WebSettings {
    std::string provider = "builtin"; // builtin | cdp | off
    int timeout_s = 20;
    int max_bytes = 1024 * 1024;
    std::string search_url = "https://html.duckduckgo.com/html/?q={query}";
};

struct HttpResponse {
    long status = 0;
    std::string body;
};

class WebTransport {
public:
    virtual ~WebTransport() = default;
    virtual std::optional<HttpResponse> get(const std::string& url, int timeout_s,
                                            int max_bytes) = 0;
};

class CurlWebTransport : public WebTransport {
public:
    std::optional<HttpResponse> get(const std::string& url, int timeout_s, int max_bytes) override;
};

class WebClient {
public:
    WebClient(WebSettings settings, std::shared_ptr<WebTransport> transport);

    std::optional<std::string> fetch(const std::string& url, std::string& error) const;
    std::vector<SearchResult> search(const std::string& query, int max_results,
                                     std::string& error) const;

private:
    WebSettings settings_;
    std::shared_ptr<WebTransport> transport_;
};

} // namespace agent::plugins::web

#endif // AGENT_PLUGINS_WEB_CLIENT_H
