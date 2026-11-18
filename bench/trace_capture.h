#ifndef BENCH_TRACE_CAPTURE_H
#define BENCH_TRACE_CAPTURE_H

// A loopback recording proxy for provider exchanges. The agent is pointed at
// `base_url()`; every chat request is forwarded to the live endpoint and the
// request bytes plus the raw response bytes are kept. `write()` scans the
// serialized trace and refuses to write it while a credential-like value
// remains: a trace is committed, so a leaked key would outlive the run.
//
// Recording is a manual act. CI never starts this proxy and never calls a live
// endpoint; the committed corpus is replayed through TraceResponder instead.

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/tool.h"

namespace bench {

struct TraceCaptureOptions {
    std::string upstream_base; // live provider base, e.g. https://api.openai.com/v1
    std::string provider;      // display name; defaults to the dialect
    std::string dialect = "openai";
    std::string source; // where this recording came from, written into the trace
};

// The name of the first credential-shaped rule `text` matches, or an empty
// string. The rule name is safe to print; the matched value is not.
std::string find_secret(const std::string& text);

// Replace credential-shaped strings (and values under a credential-shaped JSON
// key) with "[redacted]". Returns true when anything changed.
bool redact_json_secrets(agent::json& value);

// The same for raw bytes: every match is replaced in place, so a JSON body
// stays parseable and an SSE frame stays framed.
bool redact_text_secrets(std::string& text);

class TraceCapture {
public:
    explicit TraceCapture(TraceCaptureOptions options);
    ~TraceCapture();

    TraceCapture(const TraceCapture&) = delete;
    TraceCapture& operator=(const TraceCapture&) = delete;

    // Bind 127.0.0.1:0 and start the forwarding thread.
    bool start();

    // Stop accepting. Safe to call more than once.
    void stop();

    // The URL the agent's api_base should use. OpenAI appends /chat/completions
    // to the /v1 base; the versioned dialects (Anthropic, Gemini) add their own
    // version segment, so they get the root.
    std::string base_url() const;

    int exchange_count() const;

    // Write the trace, redacting first. Returns false and leaves the file
    // untouched when no exchange was captured, the path cannot be written, or a
    // credential-like value survives the scan.
    bool write(const std::string& path, std::string& err) const;

private:
    struct Exchange {
        agent::json request;
        std::string path;
        bool stream = false;
        bool request_redacted = false;
        std::string response_body;
        std::string content_type;
        int status = 0;
        bool response_redacted = false;
    };

    void serve();
    void handle(int fd);
    std::string forward_target(const std::string& target) const;
    agent::json build_document(const std::vector<Exchange>& exchanges) const;
    static agent::json exchange_json(const Exchange& exchange, int index, bool& redacted);
    std::string derive_model(const Exchange& exchange) const;
    static bool write_text(const std::string& path, const std::string& text, std::string& err);

    TraceCaptureOptions options_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex exchanges_mtx_;
    std::vector<Exchange> exchanges_;
};

} // namespace bench

#endif // BENCH_TRACE_CAPTURE_H
