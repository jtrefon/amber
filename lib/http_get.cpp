#include "agent/http_get.h"

#include <curl/curl.h>

namespace agent {

namespace {

size_t write_body(char* ptr, size_t size, size_t nmemb, void* user) {
    static_cast<std::string*>(user)->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

std::optional<std::string> http_get_with_bearer(const std::string& url, const std::string& token,
                                                long timeout_s) {
    if (url.empty() || token.empty())
        return std::nullopt;

    CURL* curl = curl_easy_init();
    if (!curl)
        return std::nullopt;

    std::string body;
    const std::string auth = "Authorization: Bearer " + token;
    curl_slist* headers = curl_slist_append(nullptr, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || status < 200 || status >= 300)
        return std::nullopt;
    return body;
}

} // namespace agent
