
#include "agent/archive_util.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <curl/curl.h>

namespace agent {

namespace {

size_t download_write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* out = static_cast<std::string*>(user);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

std::string run_capture(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), p))
        out += buf.data();
    pclose(p);
    return out;
}

} // namespace

std::string fetch_bytes(const std::string& source, std::string& err) {
    if (source.rfind("http://", 0) != 0 && source.rfind("https://", 0) != 0) {
        std::ifstream f(source, std::ios::binary);
        if (!f) {
            err = "cannot read file: " + source;
            return "";
        }
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        return body;
    }
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) {
        err = "curl init failed";
        return "";
    }
    curl_easy_setopt(c, CURLOPT_URL, source.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, download_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        err = std::string("download failed: ") + curl_easy_strerror(rc);
        return "";
    }
    if (http >= 400) {
        err = "download failed: HTTP " + std::to_string(http);
        return "";
    }
    return body;
}

std::string list_tar_gz(const std::string& archive_path) {
    return run_capture("tar -tzf " + archive_path + " 2>&1");
}

std::string unpack_tar_gz(const std::string& archive_path,
                          const std::string& dest) {
    return run_capture("tar -xzf " + archive_path + " -C " + dest + " 2>&1");
}

} // namespace agent
