
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
    if (body->size() + n > kBodyCap) return 0;
    body->append(ptr, n);
    return n;
}

int progress_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t,
                 curl_off_t) {
    auto* token = static_cast<const CancellationToken*>(userdata);
    return token->is_requested() ? 1 : 0;
}

size_t header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* reply = static_cast<HttpTransport::HttpReply*>(userdata);
    size_t n = size * nmemb;
    std::string line(ptr, n);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() &&
               (value.front() == ' ' || value.front() == '\t' ||
                value.front() == '\r' || value.front() == '\n'))
            value.erase(value.begin());
        while (!value.empty() &&
               (value.back() == ' ' || value.back() == '\t' ||
                value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        if (name == "content-type" || name == "Content-Type") {
            reply->content_type = value;
        } else if (name == "mcp-session-id" || name == "Mcp-Session-Id") {
            reply->session_header = value;
            reply->got_session_header = true;
        }
    }
    return n;
}

} // namespace

HttpTransport::HttpTransport(std::string url, std::string auth_token,
                             int request_timeout_ms,
                             std::function<void(const McpMessage&)>
                                 on_server_message,
                             const CancellationToken* cancel_token)
    : McpTransport(std::move(on_server_message)),
      url_(std::move(url)),
      auth_token_(std::move(auth_token)),
      request_timeout_ms_(request_timeout_ms > 0 ? request_timeout_ms
                                                 : 60000),
      cancel_token_(cancel_token) {}

McpTransportResult HttpTransport::request(int id, const std::string& method,
                                          const json& params) {
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
    McpTransportResult r;
    if (reply.got_session_header && session_id_.empty())
        session_id_ = reply.session_header;
    if (reply.status == 404 && !session_id_.empty()) {
        r.status = McpTransportStatus::SessionExpired;
        return r;
    }
    if (reply.status < 200 || reply.status >= 300) {
        failure_ = "mcp http " + std::to_string(reply.status) + ": " +
                   reply.body.substr(0, 512);
        r.status = McpTransportStatus::TransportError;
        return r;
    }
    if (reply.content_type.find("text/event-stream") != std::string::npos) {
        // SSE: events carry JSON-RPC messages; the response for our id
        // arrives among them, possibly after server messages.
        std::string event_data;
        std::string buf = reply.body;
        size_t pos = 0;
        while (pos < buf.size()) {
            size_t nl = buf.find('\n', pos);
            std::string line =
                (nl == std::string::npos) ? buf.substr(pos)
                                          : buf.substr(pos, nl - pos);
            pos = (nl == std::string::npos) ? buf.size() : nl + 1;
            if (line.empty()) {
                if (!event_data.empty()) {
                    auto msg = mcp_decode_line(event_data);
                    event_data.clear();
                    if (!msg) {
                        failure_ = "mcp server sent an invalid SSE message";
                        r.status = McpTransportStatus::TransportError;
                        return r;
                    }
                    if (msg->is_response() && msg->id.has_value() &&
                        msg->id->is_number_integer() &&
                        msg->id->get<int>() == id) {
                        r.message = std::move(msg);
                        return r;
                    }
                    if (on_server_message_) on_server_message_(*msg);
                }
                continue;
            }
            if (line.rfind("data:", 0) == 0) {
                std::string data = line.substr(5);
                if (!data.empty() && data.front() == ' ') data.erase(0, 1);
                if (!event_data.empty()) event_data += "\n";
                event_data += data;
            }
        }
        if (!event_data.empty()) {
            auto msg = mcp_decode_line(event_data);
            if (msg && msg->is_response() && msg->id.has_value() &&
                msg->id->is_number_integer() && msg->id->get<int>() == id) {
                r.message = std::move(msg);
                return r;
            }
            if (msg && on_server_message_) on_server_message_(*msg);
        }
        failure_ = "mcp server did not answer on the SSE stream";
        r.status = McpTransportStatus::TransportError;
        return r;
    }
    auto msg = mcp_decode_line(reply.body);
    if (!msg) {
        failure_ = "mcp server sent an invalid JSON response";
        r.status = McpTransportStatus::TransportError;
        return r;
    }
    r.message = std::move(msg);
    return r;
}

bool HttpTransport::notify(const std::string& method, const json& params) {
    HttpReply reply;
    if (!post(mcp_encode_notification(method, params), reply)) return false;
    return reply.status >= 200 && reply.status < 300;
}

bool HttpTransport::respond(int id, const json& result) {
    HttpReply reply;
    return post(mcp_encode_response(id, result), reply) &&
           reply.status >= 200 && reply.status < 300;
}

bool HttpTransport::respond_error(int id, const McpError& error) {
    HttpReply reply;
    return post(mcp_encode_error_response(id, error), reply) &&
           reply.status >= 200 && reply.status < 300;
}

void HttpTransport::close_session() {
    HttpReply reply;
    post("", reply);  // body unused; DELETE below
    (void)reply;
}

void HttpTransport::shutdown() { closed_ = true; }

std::string HttpTransport::failure_reason() const { return failure_; }

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
    HeaderList headers;
    headers.add("Content-Type: application/json");
    headers.add("Accept: application/json, text/event-stream");
    headers.add("MCP-Protocol-Version: " + std::string(kProtocolVersion));
    if (!session_id_.empty())
        headers.add("Mcp-Session-Id: " + session_id_);
    if (!auth_token_.empty())
        headers.add("Authorization: Bearer " + auth_token_);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                     static_cast<long>(request_timeout_ms_));
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &reply.body);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &reply);
    if (cancel_token_) {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA,
                         const_cast<CancellationToken*>(cancel_token_));
    }
    if (!payload.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(payload.size()));
    } else {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    CURLcode rc = curl_easy_perform(curl.get());
    if (rc == CURLE_ABORTED_BY_CALLBACK && cancel_token_ &&
        cancel_token_->is_requested()) {
        failure_ = "cancelled";
        return false;
    }
    if (rc != CURLE_OK) {
        failure_ = "mcp http transport error: " +
                   std::string(curl_easy_strerror(rc));
        return false;
    }
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &reply.status);
    return true;
}

} // namespace agent
