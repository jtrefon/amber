#ifndef AGENT_PLUGINS_WEB_PARSE_H
#define AGENT_PLUGINS_WEB_PARSE_H

#include <string>
#include <vector>

namespace agent::plugins::web {

struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

// Only http(s) URLs without credentials or control characters are accepted.
bool safe_web_url(const std::string& url, std::string& error);

// Percent-encode a query for a URL component.
std::string url_encode(const std::string& text);

std::string decode_entities(const std::string& text);

// Strip scripts/styles/tags and collapse markup whitespace.
std::string html_to_text(const std::string& html);

// Parse the result anchors from DuckDuckGo's HTML endpoint. Kept pure so the
// search provider can be tested without a network.
std::vector<SearchResult> parse_search_results(const std::string& html);

} // namespace agent::plugins::web

#endif // AGENT_PLUGINS_WEB_PARSE_H
