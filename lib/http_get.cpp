#include "agent/http_get.h"

#include <algorithm>
#include <cctype>
#include <curl/curl.h>

namespace agent {

namespace {

size_t write_body(char* ptr, size_t size, size_t nmemb, void* user) {
    static_cast<std::string*>(user)->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// Headers arrive one per callback, including the status line (no colon, so it
// is dropped). Keys are lowercased: HTTP header names are case-insensitive and
// every caller looks one up by a lowercase literal.
size_t on_header(char* buffer, size_t size, size_t nitems, void* user) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(user);
    const std::string line(buffer, size * nitems);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = lower_ascii(trim(line.substr(0, colon)));
        if (!key.empty())
            (*headers)[key] = trim(line.substr(colon + 1));
    }
    return size * nitems;
}

int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const CancellationToken*>(user)->is_requested() ? 1 : 0;
}

} // namespace

std::optional<HttpResponse> http_get(const std::string& url, const std::string& bearer,
                                     long timeout_s, const CancellationToken& cancel) {
    if (url.empty() || cancel.is_requested())
        return std::nullopt;

    CURL* curl = curl_easy_init();
    if (!curl)
        return std::nullopt;

    HttpResponse r;
    const std::string auth = "Authorization: Bearer " + bearer;
    curl_slist* headers = bearer.empty() ? nullptr : curl_slist_append(nullptr, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "amber-skill-index/1");
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    r.status = status;
    if (rc == CURLE_OPERATION_TIMEDOUT) {
        r.timed_out = true;
        return r;
    }
    if (rc != CURLE_OK)
        return std::nullopt;
    return r;
}

std::optional<std::string> http_get_with_bearer(const std::string& url, const std::string& token,
                                                long timeout_s) {
    if (url.empty() || token.empty())
        return std::nullopt;

    CancellationToken no_cancel;
    const auto r = http_get(url, token, timeout_s, no_cancel);
    if (!r || r->status < 200 || r->status >= 300)
        return std::nullopt;
    return r->body;
}

} // namespace agent
