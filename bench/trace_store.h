#ifndef BENCH_TRACE_STORE_H
#define BENCH_TRACE_STORE_H

// A recorded provider exchange replayed through the real HttpLLMClient: the
// response bytes go out verbatim, while the request is matched first. Strict
// asserts the whole request body (when one is recorded) plus path, roles, tools
// and the exact system text; shape accepts a rewritten prompt but still asserts
// the wire shape. Both fail closed on a missing, extra or drifted exchange.

#include <optional>
#include <string>
#include <vector>

#include "bench/scripted_endpoint.h"

namespace bench {

struct TraceMatchSpec {
    std::string path;
    bool stream = false;
    std::vector<std::string> roles;
    std::vector<std::string> tools;
    std::string system; // strict: exact system text; shape: required substring
};

struct TraceResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

struct TraceExchange {
    std::string id;
    TraceMatchSpec match;
    agent::json request; // strict: exact request body; empty = fingerprint only
    TraceResponse response;
};

struct Trace {
    int schema = 0;
    std::string provider;
    std::string dialect = "openai";
    std::string description;
    // The model whose path the exchange was captured against. Path-based
    // dialects (Gemini puts the model in the URL) need it to rebuild the
    // request; for OpenAI and Anthropic it is informational.
    std::string model;
    // Where the bytes came from (captured live, authored from docs, ...). Every
    // committed fixture names its origin, so a reader never mistakes a trimmed
    // example for a captured recording.
    std::string source;
    // What the recorder did before writing: "clean" (scanned, nothing found) or
    // "redacted" (a credential-like value was removed). A trace with any other
    // value is rejected rather than replayed with a secret still in it.
    std::string redaction;
    std::vector<TraceExchange> exchanges;
};

enum class TraceMode { Strict, Shape };

// "strict" | "shape"; false with mode untouched on an unknown name.
bool parse_trace_mode(const std::string& text, TraceMode& mode) noexcept;

// Parse and validate a trace file. Every error names the file and, when the
// defect is in an exchange, its index.
std::optional<Trace> load_trace(const std::string& path, std::string& err);

// The same validation for an already-parsed document; `source` names the file.
std::optional<Trace> parse_trace(const agent::json& document, const std::string& source,
                                 std::string& err);

// Serves one exchange per chat request in order. `respond` never throws: a
// mismatch or an exhausted trace answers 500 and records the first error, and
// `finish` reports an exchange that never replayed.
class TraceResponder {
public:
    TraceResponder(Trace trace, TraceMode mode, std::string source);

    EndpointReply respond(const EndpointRequest& request);

    bool failed() const noexcept { return !error_.empty(); }
    std::string error() const { return error_; }
    int served() const noexcept { return served_; }
    int total() const noexcept { return static_cast<int>(trace_.exchanges.size()); }

    // False with the first recorded error, or with the exchanges the run never
    // replayed.
    bool finish(std::string& err) const;

private:
    std::string check(const TraceExchange& exchange, const EndpointRequest& request) const;
    std::string check_wire(const TraceMatchSpec& match, const EndpointRequest& request) const;
    std::string check_messages(const TraceMatchSpec& match, const EndpointRequest& request) const;
    std::string check_system(const TraceMatchSpec& match, const EndpointRequest& request) const;

    Trace trace_;
    TraceMode mode_;
    std::string source_;
    int served_ = 0;
    std::string error_;
};

} // namespace bench

#endif // BENCH_TRACE_STORE_H
