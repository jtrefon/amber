#include "ws_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <random>
#include <sstream>

namespace cdp {

namespace {

std::string base64(const unsigned char* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? tbl[v & 63] : '=';
    }
    return out;
}

std::string random_key() {
    std::array<unsigned char, 16> key{};
    std::random_device rd;
    for (auto& b : key) b = static_cast<unsigned char>(rd() & 0xff);
    return base64(key.data(), key.size());
}

} // namespace

bool WsClient::parse_url(const std::string& url, std::string& host, int& port,
                         std::string& path) {
    if (url.rfind("ws://", 0) != 0) return false;
    std::string rest = url.substr(5);
    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        host = authority;
        port = 80;
    } else {
        host = authority.substr(0, colon);
        port = std::atoi(authority.substr(colon + 1).c_str());
    }
    return !host.empty() && port > 0;
}

bool WsClient::read_some(int timeout_ms) {
    struct pollfd pfd {fd_, POLLIN, 0};
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return false;
    std::array<char, 8192> tmp{};
    ssize_t n = recv(fd_, tmp.data(), tmp.size(), 0);
    if (n <= 0) return false;
    buf_.append(tmp.data(), (size_t)n);
    return true;
}

void WsClient::send_frame(int opcode, const std::string& payload) {
    size_t n = payload.size();
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    if (n < 126) {
        frame += static_cast<char>(0x80 | n);
    } else if (n <= 0xffff) {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>((n >> 8) & 0xff);
        frame += static_cast<char>(n & 0xff);
    } else {
        frame += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame += static_cast<char>((n >> (i * 8)) & 0xff);
    }
    std::array<unsigned char, 4> mask{};
    std::random_device rd;
    for (auto& b : mask) b = static_cast<unsigned char>(rd() & 0xff);
    frame.append(reinterpret_cast<char*>(mask.data()), 4);
    for (size_t i = 0; i < n; ++i)
        frame += static_cast<char>(payload[i] ^ mask[i % 4]);
    (void)send(fd_, frame.data(), frame.size(), 0);
}

bool WsClient::pump_frame(std::string& out) {
    // Buffer must contain: FIN/opcode + len byte.
    if (buf_.size() < 2) return false;
    unsigned char b0 = static_cast<unsigned char>(buf_[0]);
    unsigned char b1 = static_cast<unsigned char>(buf_[1]);
    int opcode = b0 & 0x0f;
    bool fin = (b0 & 0x80) != 0;
    size_t len = b1 & 0x7f;
    size_t header = 2;
    if (len == 126) {
        if (buf_.size() < 4) return false;
        len = (static_cast<unsigned char>(buf_[2]) << 8) |
              static_cast<unsigned char>(buf_[3]);
        header = 4;
    } else if (len == 127) {
        if (buf_.size() < 10) return false;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | static_cast<unsigned char>(buf_[2 + i]);
        header = 10;
    }
    // Server frames are unmasked.
    if (buf_.size() < header + len) return false;
    std::string payload = buf_.substr(header, len);
    buf_.erase(0, header + len);

    if (opcode == 0x8) return false;          // close
    if (opcode == 0x9) {                      // ping → pong
        send_frame(0xA, payload);
        return false;
    }
    if (opcode != 0x1 && opcode != 0x0) return false;
    out += payload;
    return fin;
}

bool WsClient::recv_text(std::string& out, int timeout_ms) {
    std::string frame;
    while (true) {
        frame.clear();
        if (pump_frame(frame)) { out = frame; return true; }
        if (!read_some(timeout_ms)) return false;
    }
}

bool WsClient::http_get(const std::string& host, int port,
                        const std::string& req_path, std::string& body,
                        std::string& err) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err = "socket failed"; return false; }
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) { ::close(fd); err = "unknown host " + host; return false; }
    struct sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    std::memcpy(&sa.sin_addr, he->h_addr, he->h_length);
    auto* sa_ptr = reinterpret_cast<struct sockaddr*>(&sa);
    if (::connect(fd, sa_ptr, sizeof sa) != 0) {
        ::close(fd);
        err = "connect failed";
        return false;
    }
    std::string req = "GET " + req_path + " HTTP/1.1\r\nHost: " + host + ":" +
                      std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    (void)send(fd, req.data(), req.size(), 0);
    std::string resp;
    std::array<char, 8192> tmp{};
    struct pollfd pfd {fd, POLLIN, 0};
    while (poll(&pfd, 1, 5000) > 0) {
        ssize_t n = recv(fd, tmp.data(), tmp.size(), 0);
        if (n <= 0) break;
        resp.append(tmp.data(), (size_t)n);
    }
    ::close(fd);
    size_t split = resp.find("\r\n\r\n");
    if (split == std::string::npos) { err = "malformed http response"; return false; }
    body = resp.substr(split + 4);
    return true;
}

bool WsClient::connect(const std::string& url, std::string& err) {
    std::string host;
    int port = 0;
    std::string path;
    if (!parse_url(url, host, port, path)) {
        err = "invalid ws url: " + url;
        return false;
    }
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { err = "socket failed"; return false; }
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        err = "unknown host " + host;
        close();
        return false;
    }
    struct sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    std::memcpy(&sa.sin_addr, he->h_addr, he->h_length);
    auto* sa_ptr = reinterpret_cast<struct sockaddr*>(&sa);
    if (::connect(fd_, sa_ptr, sizeof sa) != 0) {
        err = "connect to " + url + " failed";
        close();
        return false;
    }
    std::string key = random_key();
    std::string handshake = "GET " + path + " HTTP/1.1\r\n"
                            "Host: " + host + ":" + std::to_string(port) + "\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: " + key + "\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n";
    (void)send(fd_, handshake.data(), handshake.size(), 0);
    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos) {
        std::array<char, 1024> tmp{};
        ssize_t n = recv(fd_, tmp.data(), tmp.size(), 0);
        if (n <= 0) { err = "handshake failed"; close(); return false; }
        resp.append(tmp.data(), (size_t)n);
    }
    if (resp.find(" 101 ") == std::string::npos) {
        err = "handshake rejected: " + resp.substr(0, resp.find("\r\n"));
        close();
        return false;
    }
    return true;
}

bool WsClient::http_get_url(const std::string& url, std::string& body,
                            std::string& err) {
    std::string host;
    int port = 0;
    std::string path;
    if (url.rfind("http://", 0) == 0) {
        std::string rest = url.substr(7);
        size_t slash = rest.find('/');
        std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        path = (slash == std::string::npos) ? "/" : rest.substr(slash);
        size_t colon = authority.rfind(':');
        host = (colon == std::string::npos) ? authority : authority.substr(0, colon);
        port = (colon == std::string::npos) ? 80 : std::atoi(authority.substr(colon + 1).c_str());
        return http_get(host, port, path, body, err);
    }
    if (url.rfind("ws://", 0) == 0) {
        std::string rest = url.substr(5);
        size_t slash = rest.find('/');
        std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        path = (slash == std::string::npos) ? "/" : rest.substr(slash);
        size_t colon = authority.rfind(':');
        host = (colon == std::string::npos) ? authority : authority.substr(0, colon);
        port = (colon == std::string::npos) ? 80 : std::atoi(authority.substr(colon + 1).c_str());
        return http_get(host, port, path, body, err);
    }
    err = "unsupported url: " + url;
    return false;
}

bool WsClient::send_text(const std::string& payload) {
    if (fd_ < 0) return false;
    send_frame(0x1, payload);
    return true;
}

void WsClient::close() {
    if (fd_ >= 0) {
        send_frame(0x8, "");
        ::close(fd_);
        fd_ = -1;
    }
    buf_.clear();
}

} // namespace cdp
