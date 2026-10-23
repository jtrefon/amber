#include "web_client.h"

#include <algorithm>
#include <cctype>
#include <curl/curl.h>

namespace agent::plugins::web {

namespace {

struct BodyBuffer {
    std::string body;
    std::size_t limit = 0;
};

std::size_t write_body(char* data, std::size_t size, std::size_t count, void* user) {
    auto* buffer = static_cast<BodyBuffer*>(user);
    const std::size_t bytes = size * count;
    if (buffer->body.size() < buffer->limit) {
        const std::size_t room = buffer->limit - buffer->body.size();
        buffer->body.append(data, std::min(bytes, room));
    }
    return bytes;
}

std::string cap_utf8(const std::string& text, std::size_t limit) {
    if (text.size() <= limit)
        return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        --end;
    return text.substr(0, end);
}

} // namespace

std::optional<HttpResponse> CurlWebTransport::get(const std::string& url, int timeout_s,
                                                  int max_bytes) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return std::nullopt;
    BodyBuffer buffer;
    buffer.limit = static_cast<std::size_t>(std::max(1024, max_bytes));
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "amber-web/0.7");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(std::max(1, timeout_s)));
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(buffer.limit));
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        return std::nullopt;
    return HttpResponse{status, std::move(buffer.body)};
}

WebClient::WebClient(WebSettings settings, std::shared_ptr<WebTransport> transport)
    : settings_(std::move(settings)), transport_(std::move(transport)) {}

std::optional<std::string> WebClient::fetch(const std::string& url, std::string& error) const {
    if (!safe_web_url(url, error))
        return std::nullopt;
    if (!transport_) {
        error = "web transport is unavailable";
        return std::nullopt;
    }
    const std::optional<HttpResponse> response =
        transport_->get(url, settings_.timeout_s, settings_.max_bytes);
    if (!response) {
        error = "request failed";
        return std::nullopt;
    }
    if (response->status < 200 || response->status >= 300) {
        error = "HTTP " + std::to_string(response->status);
        return std::nullopt;
    }
    const std::string text = html_to_text(response->body);
    if (text.empty()) {
        error = "response contained no readable text";
        return std::nullopt;
    }
    return cap_utf8(text, static_cast<std::size_t>(std::max(1024, settings_.max_bytes)));
}

std::vector<SearchResult> WebClient::search(const std::string& query, int max_results,
                                            std::string& error) const {
    if (query.empty()) {
        error = "query is empty";
        return {};
    }
    if (!transport_) {
        error = "web transport is unavailable";
        return {};
    }
    std::string url = settings_.search_url;
    const std::string encoded = url_encode(query);
    const std::size_t placeholder = url.find("{query}");
    if (placeholder != std::string::npos)
        url.replace(placeholder, 7, encoded);
    else
        url += (url.find('?') == std::string::npos ? "?q=" : "&q=") + encoded;
    if (!safe_web_url(url, error))
        return {};

    const std::optional<HttpResponse> response =
        transport_->get(url, settings_.timeout_s, settings_.max_bytes);
    if (!response) {
        error = "search request failed";
        return {};
    }
    if (response->status < 200 || response->status >= 300) {
        error = "search returned HTTP " + std::to_string(response->status);
        return {};
    }
    auto results = parse_search_results(response->body);
    if (results.empty())
        error = "no search results parsed";
    if (max_results > 0 && static_cast<int>(results.size()) > max_results)
        results.resize(static_cast<std::size_t>(max_results));
    return results;
}

} // namespace agent::plugins::web
