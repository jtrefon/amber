#include "bench/trace_capture.h"

#include <curl/curl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <utility>

#include "bench/http_wire.h"
#include "bench/trace_store.h"

namespace bench {

namespace {

namespace fs = std::filesystem;

struct SecretRule {
    const char* name;
    const char* pattern;
};

// Prefixes and headers that are credentials by construction. A generic
// high-entropy heuristic is deliberately absent: it would flag base64 content
// and teach people to bypass the scan.
const SecretRule kSecretRules[] = {
    {"openai-key", R"(sk-[A-Za-z0-9_-]{16,})"},
    {"aws-access-key", R"(AKIA[0-9A-Z]{16})"},
    {"google-api-key", R"(AIza[0-9A-Za-z_-]{35})"},
    {"github-token", R"(gh[pousr]_[A-Za-z0-9]{20,})"},
    {"slack-token", R"(xox[baprs]-[A-Za-z0-9-]{10,})"},
    {"private-key", R"(-----BEGIN [A-Z ]*PRIVATE KEY-----)"},
    {"bearer-token", R"(Bearer[ \t]+[A-Za-z0-9._~+/=-]{16,})"},
};

const std::vector<std::pair<std::string, std::regex>>& secret_rules() {
    static const std::vector<std::pair<std::string, std::regex>> rules = [] {
        std::vector<std::pair<std::string, std::regex>> out;
        for (const SecretRule& rule : kSecretRules)
            out.emplace_back(rule.name, std::regex(rule.pattern));
        return out;
    }();
    return rules;
}

bool sensitive_key(const std::string& key) {
    static const char* kKeys[] = {"api_key",      "api-key",       "apikey",        "authorization",
                                  "access_token", "refresh_token", "client_secret", "secret",
                                  "password",     "cookie",        "set-cookie",    "x-api-key"};
    const std::string lowered = http_wire::lower(key);
    return std::any_of(std::begin(kKeys), std::end(kKeys),
                       [&lowered](const char* known) { return lowered == known; });
}

std::string response_content_type(const std::string& raw) {
    const std::size_t semi = raw.find(';');
    return http_wire::trim(semi == std::string::npos ? raw : raw.substr(0, semi));
}

std::size_t curl_write(char* data, std::size_t size, std::size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    body->append(data, size * count);
    return size * count;
}

struct HeaderSink {
    std::string content_type;
};

std::size_t curl_header(char* data, std::size_t size, std::size_t count, void* user) {
    auto* sink = static_cast<HeaderSink*>(user);
    const std::string line(data, size * count);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos &&
        http_wire::lower(http_wire::trim(line.substr(0, colon))) == "content-type")
        sink->content_type = http_wire::trim(line.substr(colon + 1));
    return size * count;
}

// One forwarded request's result, before it is turned into a trace exchange.
struct UpstreamReply {
    int status = 0;
    std::string content_type;
    std::string body;
    bool ok = false;
};

struct curl_slist* forward_headers(const std::map<std::string, std::string>& headers) {
    struct curl_slist* list = nullptr;
    for (const auto& [key, value] : headers) {
        if (key == "host" || key == "content-length" || key == "connection" ||
            key == "accept-encoding" || key == "transfer-encoding" || key == "expect")
            continue;
        std::string header = key;
        header += ": ";
        header += value;
        list = curl_slist_append(list, header.c_str());
    }
    return list;
}

UpstreamReply forward(const std::string& url, const std::string& method, const std::string& body,
                      const std::map<std::string, std::string>& headers) {
    UpstreamReply out;
    CURL* curl = curl_easy_init();
    if (!curl)
        return out;
    struct curl_slist* list = forward_headers(headers);
    HeaderSink sink;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out.status = static_cast<int>(status);
        out.content_type = response_content_type(sink.content_type);
        out.ok = true;
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return out;
}

std::string error_reply_body(const std::string& message) {
    return agent::json{{"error", {{"message", message}}}}.dump();
}

} // namespace

std::string find_secret(const std::string& text) {
    for (const auto& [name, rule] : secret_rules())
        if (std::regex_search(text, rule))
            return name;
    return {};
}

bool redact_text_secrets(std::string& text) {
    bool changed = false;
    for (const auto& [name, rule] : secret_rules()) {
        std::string replaced = std::regex_replace(text, rule, "[redacted]");
        if (replaced != text) {
            text = std::move(replaced);
            changed = true;
        }
    }
    return changed;
}

bool redact_json_secrets(agent::json& value) {
    if (value.is_object()) {
        bool changed = false;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.value().is_string() && sensitive_key(it.key()) &&
                !it.value().get<std::string>().empty()) {
                it.value() = "[redacted]";
                changed = true;
                continue;
            }
            if (redact_json_secrets(it.value()))
                changed = true;
        }
        return changed;
    }
    if (value.is_array()) {
        bool changed = false;
        for (auto& item : value)
            if (redact_json_secrets(item))
                changed = true;
        return changed;
    }
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (redact_text_secrets(text)) {
            value = text;
            return true;
        }
    }
    return false;
}

TraceCapture::TraceCapture(TraceCaptureOptions options) : options_(std::move(options)) {
    while (!options_.upstream_base.empty() && options_.upstream_base.back() == '/')
        options_.upstream_base.pop_back();
    if (options_.provider.empty())
        options_.provider = options_.dialect;
    if (options_.source.empty())
        options_.source =
            "Recorded live through the amber-bench loopback recorder; manual, never CI.";
}

TraceCapture::~TraceCapture() {
    stop();
}

bool TraceCapture::start() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        return false;
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0 ||
        listen(listen_fd_, 8) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
    return true;
}

void TraceCapture::stop() {
    stopping_ = true;
    if (thread_.joinable())
        thread_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

std::string TraceCapture::base_url() const {
    const std::string root = "http://127.0.0.1:" + std::to_string(port_);
    return options_.dialect == "openai" ? root + "/v1" : root;
}

int TraceCapture::exchange_count() const {
    std::scoped_lock lock(exchanges_mtx_);
    return static_cast<int>(exchanges_.size());
}

std::string TraceCapture::forward_target(const std::string& target) const {
    std::string path = http_wire::request_path(target);
    const std::string query = target.substr(path.size());
    for (const char* version : {"/v1beta", "/v1alpha", "/v1"}) {
        const std::size_t length = std::strlen(version);
        if (options_.upstream_base.size() < length ||
            options_.upstream_base.compare(options_.upstream_base.size() - length, length,
                                           version) != 0)
            continue;
        if (path.rfind(version, 0) == 0)
            path = path.substr(length);
        break;
    }
    return options_.upstream_base + path + query;
}

void TraceCapture::serve() {
    while (!stopping_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd_, &rfds);
        timeval tv{0, 200000};
        if (select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0)
            continue;
        const int client = accept(listen_fd_, nullptr, nullptr);
        if (client < 0)
            continue;
        handle(client);
        close(client);
    }
}

void TraceCapture::handle(int fd) {
    const std::string raw = http_wire::read_request(fd);
    if (raw.empty())
        return;
    const std::string target = http_wire::request_target(raw);
    const std::string method = http_wire::request_method(raw);
    const std::string body = http_wire::request_body(raw);
    const auto headers = http_wire::request_headers(raw);
    const std::string path = http_wire::request_path(target);
    const UpstreamReply reply = forward(forward_target(target), method, body, headers);
    if (!reply.ok) {
        http_wire::send_all(fd, error_reply_body("recording proxy: upstream request failed"),
                            "application/json", 502);
        return;
    }
    if (endpoint_is_chat_path(path)) {
        Exchange exchange;
        exchange.path = path;
        exchange.status = reply.status;
        exchange.content_type =
            reply.content_type.empty() ? "application/json" : reply.content_type;
        exchange.response_body = reply.body;
        exchange.response_redacted = redact_text_secrets(exchange.response_body);
        exchange.request = agent::json::parse(body, nullptr, false);
        if (exchange.request.is_discarded())
            exchange.request = agent::json::object();
        exchange.request_redacted = redact_json_secrets(exchange.request);
        exchange.stream = target.find("streamGenerateContent") != std::string::npos ||
                          target.find("alt=sse") != std::string::npos ||
                          (exchange.request.is_object() && exchange.request.value("stream", false));
        std::scoped_lock lock(exchanges_mtx_);
        exchanges_.push_back(std::move(exchange));
    }
    http_wire::send_all(fd, reply.body,
                        reply.content_type.empty() ? "application/json" : reply.content_type,
                        reply.status);
}

std::string TraceCapture::derive_model(const Exchange& exchange) const {
    if (exchange.request.is_object()) {
        const std::string model = exchange.request.value("model", std::string());
        if (!model.empty())
            return model;
    }
    // Gemini carries the model in the path, not the body.
    const std::string marker = "/models/";
    const std::size_t at = exchange.path.find(marker);
    if (at == std::string::npos)
        return {};
    const std::size_t begin = at + marker.size();
    const std::size_t end = exchange.path.find(':', begin);
    return exchange.path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

agent::json TraceCapture::exchange_json(const Exchange& exchange, int index, bool& redacted) {
    agent::json item = agent::json::object();
    item["id"] = "exchange-" + std::to_string(index);
    // The full system prompt carries the volatile environment card (cwd, date),
    // so a recorded trace names its stable opening line instead. Shape replay
    // asserts that marker; strict replay of a recording is possible only in an
    // identical workspace, which is why the corpus is replayed in shape mode.
    std::string system = endpoint_system_text(exchange.request);
    const std::size_t newline = system.find('\n');
    if (newline != std::string::npos)
        system.resize(newline);
    if (system.size() > 200)
        system.resize(200);
    item["match"] = agent::json{{"path", exchange.path},
                                {"stream", exchange.stream},
                                {"roles", endpoint_roles(exchange.request)},
                                {"tools", endpoint_tool_names(exchange.request)},
                                {"system", system}};
    if (!exchange.request_redacted)
        item["request"] = exchange.request;
    item["response"] = agent::json{{"status", exchange.status},
                                   {"content_type", exchange.content_type},
                                   {"body", exchange.response_body}};
    redacted = redacted || exchange.request_redacted || exchange.response_redacted;
    return item;
}

agent::json TraceCapture::build_document(const std::vector<Exchange>& exchanges) const {
    agent::json document = agent::json::object();
    document["schema"] = 1;
    document["provider"] = options_.provider;
    document["dialect"] = options_.dialect;
    document["description"] = "Captured through the amber-bench loopback recorder.";
    document["source"] = options_.source;
    const std::string model = derive_model(exchanges.front());
    if (!model.empty())
        document["model"] = model;
    bool redacted = false;
    agent::json list = agent::json::array();
    int index = 0;
    for (const Exchange& exchange : exchanges)
        list.push_back(exchange_json(exchange, ++index, redacted));
    document["redaction"] = redacted ? "redacted" : "clean";
    document["exchanges"] = std::move(list);
    return document;
}

bool TraceCapture::write_text(const std::string& path, const std::string& text, std::string& err) {
    const fs::path target(path);
    const fs::path temporary = target.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) {
            err = "cannot write " + temporary.string();
            return false;
        }
        out << text;
        if (!out) {
            err = "cannot write " + temporary.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, target, ec);
    if (ec) {
        fs::remove(temporary, ec);
        err = "cannot replace " + path;
        return false;
    }
    return true;
}

bool TraceCapture::write(const std::string& path, std::string& err) const {
    std::vector<Exchange> exchanges;
    {
        std::scoped_lock lock(exchanges_mtx_);
        exchanges = exchanges_;
    }
    if (exchanges.empty()) {
        err = "the recorder captured no exchanges (did the run reach the provider?)";
        return false;
    }
    const agent::json document = build_document(exchanges);
    const std::string text = document.dump(2) + "\n";
    const std::string rule = find_secret(text);
    if (!rule.empty()) {
        err = "refusing to write " + path + ": the trace still matches credential rule '" + rule +
              "'";
        return false;
    }
    std::string parse_error;
    if (!parse_trace(document, path, parse_error)) {
        err = parse_error;
        return false;
    }
    return write_text(path, text, err);
}

} // namespace bench
