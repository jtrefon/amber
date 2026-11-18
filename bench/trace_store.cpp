#include "bench/trace_store.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

namespace bench {

namespace {

constexpr const char* kDialects[] = {"openai", "anthropic", "gemini"};
constexpr const char* kRootKeys[] = {"schema", "provider", "dialect",   "description",
                                     "model",  "source",   "redaction", "exchanges"};
constexpr const char* kExchangeKeys[] = {"id", "match", "request", "response"};
constexpr const char* kMatchKeys[] = {"path", "stream", "roles", "tools", "system"};
constexpr const char* kResponseKeys[] = {"status", "content_type", "body"};

std::string truncate(std::string text, std::size_t limit = 120) {
    if (text.size() <= limit)
        return text;
    return text.substr(0, limit) + "...";
}

std::string describe(const agent::json& value) {
    if (value.is_string())
        return "'" + truncate(value.get<std::string>()) + "'";
    if (value.is_null())
        return "null";
    if (value.is_object())
        return "an object";
    if (value.is_array())
        return "an array";
    return truncate(value.dump());
}

std::string join_path(const std::string& base, const std::string& key) {
    return base.empty() ? key : base + "." + key;
}

void diff_object(const agent::json& expected, const agent::json& actual, const std::string& path,
                 std::string& out);
void diff_array(const agent::json& expected, const agent::json& actual, const std::string& path,
                std::string& out);

void first_difference(const agent::json& expected, const agent::json& actual,
                      const std::string& path, std::string& out) {
    if (!out.empty())
        return;
    if (expected.type() != actual.type()) {
        out = path + ": expected " + describe(expected) + ", got " + describe(actual);
        return;
    }
    if (expected.is_object()) {
        diff_object(expected, actual, path, out);
        return;
    }
    if (expected.is_array()) {
        diff_array(expected, actual, path, out);
        return;
    }
    if (expected != actual)
        out = path + ": expected " + describe(expected) + ", got " + describe(actual);
}

void diff_object(const agent::json& expected, const agent::json& actual, const std::string& path,
                 std::string& out) {
    for (auto it = expected.begin(); it != expected.end() && out.empty(); ++it) {
        if (!actual.contains(it.key())) {
            out = join_path(path, it.key()) + ": missing";
            return;
        }
        first_difference(it.value(), actual[it.key()], join_path(path, it.key()), out);
    }
    for (auto it = actual.begin(); it != actual.end() && out.empty(); ++it)
        if (!expected.contains(it.key()))
            out = join_path(path, it.key()) + ": unexpected";
}

void diff_array(const agent::json& expected, const agent::json& actual, const std::string& path,
                std::string& out) {
    if (expected.size() != actual.size()) {
        out = path + ": expected " + std::to_string(expected.size()) + " item(s), got " +
              std::to_string(actual.size());
        return;
    }
    for (std::size_t i = 0; i < expected.size() && out.empty(); ++i)
        first_difference(expected[i], actual[i], path + "[" + std::to_string(i) + "]", out);
}

std::string roles_text(const std::vector<std::string>& roles) {
    std::string out = "[";
    for (const std::string& role : roles) {
        if (out.size() > 1)
            out += ", ";
        out += role;
    }
    return out + "]";
}

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

std::string normalize_eol(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;
        out += text[i];
    }
    return out;
}

bool validate_sse_line(const std::string& line, bool& has_data, std::string& err) {
    if (line.empty() || line[0] == ':')
        return true;
    if (line.rfind("data:", 0) == 0) {
        has_data = true;
        const std::string payload = trim(line.substr(5));
        if (payload != "[DONE]" && agent::json::parse(payload, nullptr, false).is_discarded()) {
            err = "SSE data is not JSON: " + truncate(payload);
            return false;
        }
        return true;
    }
    if (line.rfind("event:", 0) == 0 || line.rfind("id:", 0) == 0 || line.rfind("retry:", 0) == 0)
        return true;
    err = "unknown SSE field: " + truncate(line);
    return false;
}

bool validate_sse_frame(const std::string& frame, int index, std::string& err) {
    bool has_data = false;
    bool has_comment = false;
    std::size_t start = 0;
    while (start <= frame.size()) {
        const std::size_t end = frame.find('\n', start);
        const std::string line =
            frame.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!validate_sse_line(line, has_data, err)) {
            std::string detail = "SSE frame " + std::to_string(index) + ": ";
            detail += err;
            err = detail;
            return false;
        }
        has_comment = has_comment || (!line.empty() && line[0] == ':');
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (!has_data && !has_comment) {
        err = "SSE frame " + std::to_string(index) + " has no data";
        return false;
    }
    return true;
}

bool validate_sse(const std::string& body, std::string& err) {
    const std::string text = normalize_eol(body);
    if (text.size() < 2 || text.compare(text.size() - 2, 2, "\n\n") != 0) {
        err = "SSE body must end with a blank line";
        return false;
    }
    std::size_t pos = 0;
    int index = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find("\n\n", pos);
        const std::string frame =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!frame.empty() && !validate_sse_frame(frame, index, err))
            return false;
        if (!frame.empty())
            ++index;
        if (end == std::string::npos)
            break;
        pos = end + 2;
    }
    return true;
}

bool has_only_keys(const agent::json& object, const char* const* keys, std::size_t count,
                   std::string& err) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (std::size_t i = 0; i < count; ++i)
            known = known || it.key() == keys[i];
        if (!known) {
            err = "unknown field '" + it.key() + "'";
            return false;
        }
    }
    return true;
}

template <std::size_t N>
bool has_only_keys(const agent::json& object, const char* const (&keys)[N], std::string& err) {
    return has_only_keys(object, keys, N, err);
}

bool string_array(const agent::json& value, std::vector<std::string>& out, std::string& err) {
    if (!value.is_array()) {
        err = "must be an array of strings";
        return false;
    }
    for (const auto& item : value) {
        if (!item.is_string()) {
            err = "must be an array of strings";
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

bool parse_match(const agent::json& value, TraceMatchSpec& out, std::string& err) {
    if (!value.is_object()) {
        err = "'match' must be an object";
        return false;
    }
    if (!has_only_keys(value, kMatchKeys, err))
        return false;
    out.path = value.value("path", std::string());
    if (value.contains("stream") && !value["stream"].is_boolean()) {
        err = "'match.stream' must be a boolean";
        return false;
    }
    out.stream = value.value("stream", false);
    if (value.contains("roles") && !string_array(value["roles"], out.roles, err))
        return false;
    if (value.contains("tools") && !string_array(value["tools"], out.tools, err))
        return false;
    if (value.contains("system") && !value["system"].is_string()) {
        err = "'match.system' must be a string";
        return false;
    }
    out.system = value.value("system", std::string());
    return true;
}

bool parse_status(const agent::json& value, int& status, std::string& err) {
    if (!value.contains("status"))
        return true;
    if (!value["status"].is_number_integer() || value["status"].get<int>() < 100 ||
        value["status"].get<int>() > 599) {
        err = "'response.status' must be an HTTP status";
        return false;
    }
    status = value["status"].get<int>();
    return true;
}

bool valid_response_body(const std::string& content_type, const std::string& body,
                         std::string& err) {
    if (content_type == "text/event-stream")
        return validate_sse(body, err);
    if (content_type.find("json") != std::string::npos &&
        agent::json::parse(body, nullptr, false).is_discarded()) {
        err = "'response.body' is not JSON";
        return false;
    }
    return true;
}

bool parse_response(const agent::json& value, TraceResponse& out, std::string& err) {
    if (!value.is_object()) {
        err = "'response' must be an object";
        return false;
    }
    if (!has_only_keys(value, kResponseKeys, err))
        return false;
    if (!value.contains("body") || !value["body"].is_string() ||
        value["body"].get<std::string>().empty()) {
        err = "'response.body' must be a non-empty string";
        return false;
    }
    out.body = value["body"].get<std::string>();
    if (!parse_status(value, out.status, err))
        return false;
    if (value.contains("content_type") && !value["content_type"].is_string()) {
        err = "'response.content_type' must be a string";
        return false;
    }
    out.content_type = value.value("content_type", std::string("application/json"));
    return valid_response_body(out.content_type, out.body, err);
}

bool parse_exchange(const agent::json& value, TraceExchange& out, std::string& err) {
    if (!value.is_object()) {
        err = "must be an object";
        return false;
    }
    if (!has_only_keys(value, kExchangeKeys, err))
        return false;
    if (value.contains("id") && !value["id"].is_string()) {
        err = "'id' must be a string";
        return false;
    }
    out.id = value.value("id", std::string());
    if (value.contains("match") && !parse_match(value["match"], out.match, err))
        return false;
    if (value.contains("request")) {
        if (!value["request"].is_object()) {
            err = "'request' must be an object";
            return false;
        }
        out.request = value["request"];
    }
    if (!value.contains("response")) {
        err = "missing 'response'";
        return false;
    }
    return parse_response(value["response"], out.response, err);
}

std::string exchange_error(const std::string& source, std::size_t index, const std::string& err) {
    return source + ": exchange " + std::to_string(index) + ": " + err;
}

bool collect_exchange(const agent::json& value, const std::string& source, std::size_t index,
                      Trace& trace, std::set<std::string>& ids, std::string& err) {
    TraceExchange exchange;
    std::string detail;
    if (!parse_exchange(value, exchange, detail)) {
        err = exchange_error(source, index, detail);
        return false;
    }
    if (!exchange.id.empty() && !ids.insert(exchange.id).second) {
        err = exchange_error(source, index, "duplicate id '" + exchange.id + "'");
        return false;
    }
    trace.exchanges.push_back(std::move(exchange));
    return true;
}

bool valid_dialect(const std::string& dialect) {
    return std::any_of(std::begin(kDialects), std::end(kDialects),
                       [&dialect](const char* known) { return dialect == known; });
}

} // namespace

bool parse_trace_mode(const std::string& text, TraceMode& mode) noexcept {
    if (text == "strict") {
        mode = TraceMode::Strict;
        return true;
    }
    if (text == "shape") {
        mode = TraceMode::Shape;
        return true;
    }
    return false;
}

bool parse_trace_identity(const agent::json& document, const std::string& source, Trace& trace,
                          std::string& err) {
    if (!document.contains("schema") || !document["schema"].is_number_integer() ||
        document["schema"].get<int>() != 1) {
        err = source + ": 'schema' must be the integer 1";
        return false;
    }
    trace.schema = 1;
    trace.dialect = document.value("dialect", std::string());
    if (!valid_dialect(trace.dialect)) {
        err = source + ": unknown dialect '" + trace.dialect + "'";
        return false;
    }
    if (document.contains("provider") && !document["provider"].is_string()) {
        err = source + ": 'provider' must be a string";
        return false;
    }
    trace.provider = document.value("provider", trace.dialect);
    if (document.contains("description") && !document["description"].is_string()) {
        err = source + ": 'description' must be a string";
        return false;
    }
    if (document.contains("model") && !document["model"].is_string()) {
        err = source + ": 'model' must be a string";
        return false;
    }
    trace.model = document.value("model", std::string());
    return true;
}

bool parse_trace_provenance(const agent::json& document, const std::string& source, Trace& trace,
                            std::string& err) {
    if (!document.contains("source") || !document["source"].is_string() ||
        document["source"].get<std::string>().empty()) {
        err = source + ": 'source' must be a non-empty string";
        return false;
    }
    trace.source = document["source"].get<std::string>();
    if (!document.contains("redaction") || !document["redaction"].is_string()) {
        err = source + ": 'redaction' must be 'clean' or 'redacted'";
        return false;
    }
    trace.redaction = document["redaction"].get<std::string>();
    if (trace.redaction != "clean" && trace.redaction != "redacted") {
        err = source + ": 'redaction' must be 'clean' or 'redacted'";
        return false;
    }
    return true;
}

bool parse_trace_header(const agent::json& document, const std::string& source, Trace& trace,
                        std::string& err) {
    std::string detail;
    if (!has_only_keys(document, kRootKeys, detail)) {
        err = source + ": " + detail;
        return false;
    }
    return parse_trace_identity(document, source, trace, err) &&
           parse_trace_provenance(document, source, trace, err);
}

bool parse_exchanges(const agent::json& document, const std::string& source, Trace& trace,
                     std::string& err) {
    if (!document.contains("exchanges") || !document["exchanges"].is_array() ||
        document["exchanges"].empty()) {
        err = source + ": 'exchanges' must be a non-empty array";
        return false;
    }
    std::set<std::string> ids;
    std::size_t index = 0;
    for (const auto& exchange : document["exchanges"]) {
        if (!collect_exchange(exchange, source, index, trace, ids, err))
            return false;
        ++index;
    }
    return true;
}

std::optional<Trace> parse_trace(const agent::json& document, const std::string& source,
                                 std::string& err) {
    if (!document.is_object()) {
        err = source + ": trace must be a JSON object";
        return std::nullopt;
    }
    Trace trace;
    if (!parse_trace_header(document, source, trace, err) ||
        !parse_exchanges(document, source, trace, err))
        return std::nullopt;
    return trace;
}

std::optional<Trace> load_trace(const std::string& path, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = path + ": cannot read the trace file";
        return std::nullopt;
    }
    const agent::json document = agent::json::parse(in, nullptr, false);
    if (document.is_discarded()) {
        err = path + ": trace is not valid JSON";
        return std::nullopt;
    }
    return parse_trace(document, path, err);
}

TraceResponder::TraceResponder(Trace trace, TraceMode mode, std::string source)
    : trace_(std::move(trace)), mode_(mode), source_(std::move(source)) {}

std::string TraceResponder::check_wire(const TraceMatchSpec& match,
                                       const EndpointRequest& request) const {
    if (!match.path.empty() && match.path != request.path)
        return "path: expected '" + match.path + "', got '" + request.path + "'";
    if (match.stream != request.stream)
        return std::string("stream: expected ") + (match.stream ? "true" : "false") + ", got " +
               (request.stream ? "true" : "false");
    return {};
}

std::string TraceResponder::check_messages(const TraceMatchSpec& match,
                                           const EndpointRequest& request) const {
    const std::vector<std::string> roles = endpoint_roles(request.json);
    if (!match.roles.empty() && roles != match.roles)
        return "roles: expected " + roles_text(match.roles) + ", got " + roles_text(roles);
    const std::vector<std::string> tools = endpoint_tool_names(request.json);
    for (const std::string& wanted : match.tools)
        if (std::find(tools.begin(), tools.end(), wanted) == tools.end())
            return "tools: missing required tool '" + wanted + "'";
    return {};
}

std::string TraceResponder::check_system(const TraceMatchSpec& match,
                                         const EndpointRequest& request) const {
    if (match.system.empty())
        return {};
    const std::string system = endpoint_system_text(request.json);
    if (mode_ == TraceMode::Strict && system != match.system)
        return "system: expected " + describe(match.system) + ", got " + describe(system);
    if (mode_ == TraceMode::Shape && system.find(match.system) == std::string::npos)
        return "system: does not contain " + describe(match.system);
    return {};
}

std::string TraceResponder::check(const TraceExchange& exchange,
                                  const EndpointRequest& request) const {
    if (const std::string drift = check_wire(exchange.match, request); !drift.empty())
        return drift;
    if (const std::string drift = check_messages(exchange.match, request); !drift.empty())
        return drift;
    if (const std::string drift = check_system(exchange.match, request); !drift.empty())
        return drift;
    if (mode_ != TraceMode::Strict || exchange.request.empty())
        return {};
    std::string difference;
    first_difference(exchange.request, request.json, "request", difference);
    return difference;
}

EndpointReply TraceResponder::respond(const EndpointRequest& request) {
    EndpointReply reply;
    if (!endpoint_is_chat_path(request.path)) {
        reply.handled = false; // the endpoint's own /v1/models and 404 handling
        return reply;
    }
    if (error_.empty() && served_ >= static_cast<int>(trace_.exchanges.size()))
        error_ = source_ + ": request " + std::to_string(served_ + 1) + " has no exchange (" +
                 std::to_string(trace_.exchanges.size()) + " defined)";
    if (error_.empty()) {
        const TraceExchange& exchange = trace_.exchanges[static_cast<std::size_t>(served_)];
        const std::string drift = check(exchange, request);
        if (!drift.empty())
            error_ =
                source_ + ": exchange " + std::to_string(served_) + ": request drifted at " + drift;
        else
            ++served_;
    }
    if (!error_.empty()) {
        reply.status = 500;
        reply.body = agent::json{{"error", {{"message", error_}}}}.dump();
        return reply;
    }
    const TraceResponse& response =
        trace_.exchanges[static_cast<std::size_t>(served_ - 1)].response;
    reply.status = response.status;
    reply.content_type = response.content_type;
    reply.body = response.body;
    return reply;
}

bool TraceResponder::finish(std::string& err) const {
    if (!error_.empty()) {
        err = error_;
        return false;
    }
    if (served_ < static_cast<int>(trace_.exchanges.size())) {
        err = source_ + ": " + std::to_string(trace_.exchanges.size() - served_) +
              " exchange(s) unused; next is exchange " + std::to_string(served_);
        return false;
    }
    err.clear();
    return true;
}

} // namespace bench
