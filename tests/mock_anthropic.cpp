#include "mock_anthropic.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace agent::test {

namespace {

using json = nlohmann::json;

// The same 4-bytes-per-token estimate the harness uses elsewhere. The mock
// only needs a monotone size, not a real tokenizer: what the cache rules turn
// on is the ordering and the minimum.
long estimate(const std::string& text) {
    return static_cast<long>(text.size() / 4);
}

bool cacheable_block(const json& block) {
    return block.is_object() && block.contains("cache_control") &&
           block["cache_control"].is_object();
}

// One prefix position: the cumulative text up to and including a unit, and
// whether that unit carried a breakpoint.
struct PrefixPosition {
    std::string prefix;
    long tokens = 0;
    bool marked = false;
};

// Anthropic's cache order is tools -> system -> messages, and a breakpoint
// caches everything up to and including itself. The marker is stripped from
// the hashed content: adding or moving a breakpoint must not change what the
// prefix *is*, or every request would look like a different prompt.
class PrefixWalk {
public:
    void add(const json& unit, bool cacheable) {
        json copy = unit;
        if (copy.is_object())
            copy.erase("cache_control");
        acc_ += copy.dump(-1, ' ', false, json::error_handler_t::replace);
        acc_ += '\x1f';
        tokens_ = estimate(acc_);
        if (cacheable) {
            PrefixPosition p;
            p.prefix = acc_;
            p.tokens = tokens_;
            p.marked = true;
            positions_.push_back(std::move(p));
        }
    }

    void add_text(const std::string& text) { add(json(text), false); }

    // Automatic caching: the top-level field marks the last cacheable unit,
    // which is how the provider treats it when no block carries a marker.
    void mark_last() {
        PrefixPosition p;
        p.prefix = acc_;
        p.tokens = tokens_;
        p.marked = true;
        positions_.push_back(std::move(p));
    }

    long tokens() const { return tokens_; }
    std::string full() const { return acc_; }
    const std::vector<PrefixPosition>& positions() const { return positions_; }

private:
    std::string acc_;
    long tokens_ = 0;
    std::vector<PrefixPosition> positions_;
};

// Walk the wire body in cache order, recording every breakpoint.
PrefixWalk walk(const json& body) {
    PrefixWalk out;
    if (body.contains("tools") && body["tools"].is_array())
        for (const auto& tool : body["tools"])
            out.add(tool, false);

    const json system = body.value("system", json());
    if (system.is_array())
        for (const auto& block : system)
            out.add(block, cacheable_block(block));
    else if (system.is_string() && !system.get<std::string>().empty())
        out.add_text(system.get<std::string>());

    const bool automatic = body.contains("cache_control") && body["cache_control"].is_object();
    if (body.contains("messages") && body["messages"].is_array()) {
        for (const auto& msg : body["messages"]) {
            if (!msg.is_object())
                continue;
            const std::string role = msg.value("role", std::string());
            const json content = msg.value("content", json());
            if (content.is_array()) {
                for (const auto& block : content) {
                    json unit = block;
                    if (unit.is_object())
                        unit["role"] = role;
                    out.add(unit, cacheable_block(block));
                }
            } else if (content.is_string()) {
                json unit = {{"role", role}, {"content", content}};
                out.add(unit, cacheable_block(msg));
            }
        }
    }
    if (automatic)
        out.mark_last();
    return out;
}

} // namespace

MockAnthropic::~MockAnthropic() {
    stop();
}

bool MockAnthropic::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        return false;
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        return false;
    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0)
        return false;
    port_ = ntohs(addr.sin_port);
    if (listen(listen_fd_, 8) < 0)
        return false;
    thread_ = std::thread([this] { serve(); });
    return true;
}

std::string MockAnthropic::base() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

void MockAnthropic::set_min_cacheable_tokens(long tokens) {
    std::scoped_lock lk(mtx_);
    min_tokens_ = tokens;
}

std::vector<MockRequest> MockAnthropic::requests() {
    std::scoped_lock lk(mtx_);
    return requests_;
}

MockUsage MockAnthropic::last_usage() {
    std::scoped_lock lk(mtx_);
    return last_usage_;
}

void MockAnthropic::stop() {
    stopping_ = true;
    if (thread_.joinable())
        thread_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

void MockAnthropic::serve() {
    while (!stopping_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd_, &rfds);
        timeval tv{0, 200000};
        if (select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0)
            continue;
        const int conn = accept(listen_fd_, nullptr, nullptr);
        if (conn < 0)
            continue;
        handle(conn);
        close(conn);
    }
}

namespace {

// One request head: the start line plus headers, lower-cased names. The last
// header carries no trailing CRLF once the blank line is stripped, so the walk
// handles a final line without one -- missing Content-Length truncates the body
// and every JSON parse then fails for a reason that looks like the client's.
MockRequest parse_head(const std::string& head) {
    MockRequest req;
    std::size_t pos = 0;
    bool start_line = true;
    while (pos < head.size()) {
        const std::size_t end = head.find("\r\n", pos);
        const std::string line =
            head.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (start_line) {
            const std::size_t sp1 = line.find(' ');
            const std::size_t sp2 =
                sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos)
                req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
            start_line = false;
        } else if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::size_t value = colon + 1;
                while (value < line.size() && line[value] == ' ')
                    ++value;
                req.headers[name] = line.substr(value);
            }
        }
        if (end == std::string::npos)
            break;
        pos = end + 2;
    }
    return req;
}

} // namespace

void MockAnthropic::handle(int conn) {
    std::string raw;
    char buf[8192];
    std::size_t head_end;
    for (;;) {
        head_end = raw.find("\r\n\r\n");
        if (head_end != std::string::npos)
            break;
        const ssize_t n = recv(conn, buf, sizeof(buf), 0);
        if (n <= 0)
            return;
        raw.append(buf, static_cast<std::size_t>(n));
    }
    MockRequest req = parse_head(raw.substr(0, head_end));
    const std::string body_head = raw.substr(head_end + 4);
    std::size_t length = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end())
        length = static_cast<std::size_t>(std::stoul(it->second));
    req.body = body_head;
    while (req.body.size() < length) {
        const ssize_t n = recv(conn, buf, sizeof(buf), 0);
        if (n <= 0)
            return;
        req.body.append(buf, static_cast<std::size_t>(n));
    }
    {
        std::scoped_lock lk(mtx_);
        requests_.push_back(req);
    }

    json body = json::parse(req.body, nullptr, false);
    const bool stream = !body.is_discarded() && body.value("stream", false);
    const std::string response = message_json(req.body, stream);
    const std::string head = std::string("HTTP/1.1 200 OK\r\nContent-Type: ") +
                             (stream ? "text/event-stream" : "application/json") +
                             "\r\nContent-Length: " + std::to_string(response.size()) +
                             "\r\nConnection: close\r\n\r\n";
    const std::string http = head + response;
    std::size_t sent = 0;
    while (sent < http.size()) {
        const ssize_t n = send(conn, http.c_str() + sent, http.size() - sent, 0);
        if (n <= 0)
            return;
        sent += static_cast<std::size_t>(n);
    }
}

namespace {

struct Usage {
    long input = 0;
    long read = 0;
    long write = 0;
};

json usage_json(const Usage& u) {
    return {{"input_tokens", u.input},
            {"output_tokens", 1},
            {"cache_read_input_tokens", u.read},
            {"cache_creation_input_tokens", u.write}};
}

std::string sse_event(const std::string& name, const json& payload) {
    return "event: " + name + "\ndata: " + payload.dump() + "\n\n";
}

} // namespace

// The cache rules, applied to one request body, kept apart from the socket so
// the accounting stays readable: a prefix under the minimum is silently uncached,
// a hit reads the longest previously written prefix, and only the remainder is
// written.
std::string MockAnthropic::message_json(const std::string& body_text, bool stream) {
    json body = json::parse(body_text, nullptr, false);
    Usage u;
    long total = 0;
    long min_tokens = 0;
    {
        std::scoped_lock lk(mtx_);
        min_tokens = min_tokens_;
    }
    if (!body.is_discarded()) {
        PrefixWalk p = walk(body);
        total = p.tokens();
        const auto& marks = p.positions();
        const auto now = std::chrono::steady_clock::now();
        if (!marks.empty() && marks.back().tokens >= min_tokens) {
            const std::string& last = marks.back().prefix;
            for (const auto& [key, entry] : cache_) {
                if (now >= entry.expires || key.size() > last.size())
                    continue;
                if (last.compare(0, key.size(), key) == 0 && entry.tokens > u.read)
                    u.read = entry.tokens;
            }
            u.write = marks.back().tokens - u.read;
            cache_[last] = CacheEntry{marks.back().tokens, now + std::chrono::minutes(5)};
        }
    }
    u.input = std::max(0L, total - u.read - u.write);
    {
        std::scoped_lock lk(mtx_);
        last_usage_ = MockUsage{u.input, u.read, u.write};
    }

    const json message = {{"id", "msg_mock"},
                          {"type", "message"},
                          {"role", "assistant"},
                          {"model", "mock"},
                          {"content", json::array({{{"type", "text"}, {"text", "ok"}}})},
                          {"stop_reason", "end_turn"},
                          {"usage", usage_json(u)}};
    if (!stream)
        return message.dump();

    std::string out = sse_event("message_start", {{"type", "message_start"}, {"message", message}});
    out += sse_event("content_block_start", {{"type", "content_block_start"},
                                             {"index", 0},
                                             {"content_block", {{"type", "text"}, {"text", ""}}}});
    out += sse_event("content_block_delta", {{"type", "content_block_delta"},
                                             {"index", 0},
                                             {"delta", {{"type", "text_delta"}, {"text", "ok"}}}});
    out += sse_event("content_block_stop", {{"type", "content_block_stop"}, {"index", 0}});
    out += sse_event("message_delta", {{"type", "message_delta"},
                                       {"delta", {{"stop_reason", "end_turn"}}},
                                       {"usage", {{"output_tokens", 1}}}});
    out += sse_event("message_stop", {{"type", "message_stop"}});
    return out;
}

} // namespace agent::test
