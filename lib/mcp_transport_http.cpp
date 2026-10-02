
#include "agent/mcp_transport_http.h"
#include "http_transport.h"

#include <cstddef>
#include <curl/curl.h>

namespace agent {

namespace {

constexpr size_t kBodyCap = static_cast<size_t>(8) * 1024 * 1024;
constexpr const char* kProtocolVersion = "2025-06-18";

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    size_t n = size * nmemb;
    if (body->size() + n > kBodyCap)
        return 0;
    body->append(ptr, n);
    return n;
}

int progress_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* token = static_cast<const CancellationToken*>(userdata);
    return token->is_requested() ? 1 : 0;
}

// Trim the leading and trailing whitespace off an HTTP header value.
std::string trim_header_value(const std::string& raw) {
    const char* ws = " \t\r\n";
    const size_t first = raw.find_first_not_of(ws);
    if (first == std::string::npos)
        return "";
    const size_t last = raw.find_last_not_of(ws);
    return raw.substr(first, last - first + 1);
}

bool is_content_type_header(const std::string& name) {
    return name == "content-type" || name == "Content-Type";
}

bool is_session_id_header(const std::string& name) {
    return name == "mcp-session-id" || name == "Mcp-Session-Id";
}

size_t header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* reply = static_cast<HttpTransport::HttpReply*>(userdata);
    const size_t n = size * nmemb;
    const std::string line(ptr, n);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        const std::string name = line.substr(0, colon);
        const std::string value = trim_header_value(line.substr(colon + 1));
        if (is_content_type_header(name)) {
            reply->content_type = value;
        } else if (is_session_id_header(name)) {
            reply->session_header = value;
            reply->got_session_header = true;
        }
    }
    return n;
}

} // namespace

HttpTransport::HttpTransport(std::string url, std::string auth_token, int request_timeout_ms,
                             std::function<void(const McpMessage&)> on_server_message,
                             const CancellationToken* cancel_token)
    : McpTransport(std::move(on_server_message)), url_(std::move(url)),
      auth_token_(std::move(auth_token)),
      request_timeout_ms_(request_timeout_ms > 0 ? request_timeout_ms : 60000),
      cancel_token_(cancel_token) {}

// True when the message is the JSON-RPC response to `id`.
bool is_response_for(const McpMessage& msg, int id) {
    return msg.is_response() && msg.id.has_value() && msg.id->is_number_integer() &&
           msg.id->get<int>() == id;
}

// The failure result, recording why for the caller's error text.
McpTransportResult transport_error(std::string& failure, const std::string& why) {
    failure = why;
    McpTransportResult r;
    r.status = McpTransportStatus::TransportError;
    return r;
}

// Decode one accumulated SSE event. Returns the response for `id` when the event
// carried it; sets `failed` when the event was undecodable, and otherwise hands
// the message to the server-message callback.
std::optional<McpMessage> HttpTransport::dispatch_sse_event(std::string& event_data, int id,
                                                            bool& failed) {
    auto msg = mcp_decode_line(event_data);
    event_data.clear();
    if (!msg) {
        failed = true;
        return std::nullopt;
    }
    if (is_response_for(*msg, id))
        return msg;
    if (on_server_message_)
        on_server_message_(*msg);
    return std::nullopt;
}

// SSE: events carry JSON-RPC messages; the response for our id arrives among
// them, possibly after server messages.
McpTransportResult HttpTransport::handle_sse_response(const std::string& body, int id) {
    std::string event_data;
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t nl = body.find('\n', pos);
        const std::string line =
            (nl == std::string::npos) ? body.substr(pos) : body.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? body.size() : nl + 1;
        if (line.empty()) {
            if (event_data.empty())
                continue;
            bool failed = false;
            auto msg = dispatch_sse_event(event_data, id, failed);
            if (failed)
                return transport_error(failure_, "mcp server sent an invalid SSE message");
            if (msg) {
                McpTransportResult r;
                r.message = std::move(msg);
                return r;
            }
            continue;
        }
        if (line.rfind("data:", 0) == 0) {
            std::string data = line.substr(5);
            if (!data.empty() && data.front() == ' ')
                data.erase(0, 1);
            if (!event_data.empty())
                event_data += "\n";
            event_data += data;
        }
    }
    if (!event_data.empty()) {
        bool failed = false;
        auto msg = dispatch_sse_event(event_data, id, failed);
        if (msg) {
            McpTransportResult r;
            r.message = std::move(msg);
            return r;
        }
    }
    return transport_error(failure_, "mcp server did not answer on the SSE stream");
}

McpTransportResult HttpTransport::request(int id, const std::string& method, const json& params) {
    McpRequest req;
    req.id = id;
    req.method = method;
    req.params = params;
    HttpReply reply;
    if (!post(mcp_encode_request(req), reply)) {
        McpTransportResult r;
        r.status = (cancel_token_ && cancel_token_->is_requested())
                       ? McpTransportStatus::Cancelled
                       : McpTransportStatus::TransportError;
        return r;
    }
    if (reply.got_session_header && session_id_.empty())
        session_id_ = reply.session_header;
    if (reply.status == 404 && !session_id_.empty()) {
        McpTransportResult r;
        r.status = McpTransportStatus::SessionExpired;
        return r;
    }
    if (reply.status < 200 || reply.status >= 300)
        return transport_error(failure_, "mcp http " + std::to_string(reply.status) + ": " +
                                             reply.body.substr(0, 512));
    if (reply.content_type.find("text/event-stream") != std::string::npos)
        return handle_sse_response(reply.body, id);

    auto msg = mcp_decode_line(reply.body);
    if (!msg)
        return transport_error(failure_, "mcp server sent an invalid JSON response");
    McpTransportResult r;
    r.message = std::move(msg);
    return r;
}

bool HttpTransport::notify(const std::string& method, const json& params) {
    HttpReply reply;
    if (!post(mcp_encode_notification(method, params), reply))
        return false;
    return reply.status >= 200 && reply.status < 300;
}

bool HttpTransport::respond(int id, const json& result) {
    HttpReply reply;
    return post(mcp_encode_response(id, result), reply) && reply.status >= 200 &&
           reply.status < 300;
}

bool HttpTransport::respond_error(int id, const McpError& error) {
    HttpReply reply;
    return post(mcp_encode_error_response(id, error), reply) && reply.status >= 200 &&
           reply.status < 300;
}

void HttpTransport::close_session() {
    HttpReply reply;
    post("", reply); // body unused; DELETE below
    (void)reply;
}

void HttpTransport::shutdown() {
    closed_ = true;
}

std::string HttpTransport::failure_reason() const {
    return failure_;
}

// Record why the transfer failed. `aborted` means the caller cancelled.
bool HttpTransport::transfer_failed(bool aborted, const std::string& why) {
    failure_ = aborted ? "cancelled" : why;
    return false;
}

namespace {

HeaderList build_headers(const std::string& session_id, const std::string& auth_token) {
    HeaderList headers;
    headers.add("Content-Type: application/json");
    headers.add("Accept: application/json, text/event-stream");
    headers.add("MCP-Protocol-Version: " + std::string(kProtocolVersion));
    if (!session_id.empty())
        headers.add("Mcp-Session-Id: " + session_id);
    if (!auth_token.empty())
        headers.add("Authorization: Bearer " + auth_token);
    return headers;
}

void apply_curl_options(CURL* curl, const HeaderList& headers, HttpTransport::HttpReply& reply,
                        const std::string& payload, const std::string& url, long timeout_ms,
                        const CancellationToken* cancel) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &reply);
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<CancellationToken*>(cancel));
    }
    if (!payload.empty()) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
}

} // namespace

bool HttpTransport::post(const std::string& payload, HttpReply& reply) {
    if (closed_) {
        failure_ = failure_.empty() ? "transport closed" : failure_;
        return false;
    }
    CurlPtr curl = make_curl();
    if (!curl) {
        failure_ = "curl init failed";
        return false;
    }
    const HeaderList headers = build_headers(session_id_, auth_token_);
    apply_curl_options(curl.get(), headers, reply, payload, url_,
                       static_cast<long>(request_timeout_ms_), cancel_token_);

    const CURLcode rc = curl_easy_perform(curl.get());
    if (rc == CURLE_ABORTED_BY_CALLBACK && cancel_token_ && cancel_token_->is_requested())
        return transfer_failed(/*aborted=*/true, "");
    if (rc != CURLE_OK)
        return transfer_failed(/*aborted=*/false,
                               "mcp http transport error: " + std::string(curl_easy_strerror(rc)));
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &reply.status);
    return true;
}

} // namespace agent
