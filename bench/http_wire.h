#ifndef BENCH_HTTP_WIRE_H
#define BENCH_HTTP_WIRE_H

// The small, shared pieces of the loopback HTTP servers in this directory: read
// one request off a socket and parse its target, headers and body, then write a
// response. Both the scripted endpoint and the trace recorder speak the same
// wire, so the parser lives here instead of in either one.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace bench::http_wire {

inline std::string lower(std::string value) {
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

inline std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Read one HTTP request: the header block first, then exactly Content-Length
// bytes of body. A peer that closes early yields whatever arrived, which the
// caller treats as a bad request.
inline std::string read_request(int fd) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
            return {};
        request.append(buffer, static_cast<std::size_t>(n));
    }
    const std::size_t header_end = request.find("\r\n\r\n");
    std::size_t content_length = 0;
    const std::string headers = lower(request.substr(0, header_end));
    const std::size_t cl = headers.find("content-length:");
    if (cl != std::string::npos) {
        const std::size_t start = cl + std::strlen("content-length:");
        content_length =
            static_cast<std::size_t>(std::strtoul(headers.c_str() + start, nullptr, 10));
    }
    while (request.size() < header_end + 4 + content_length) {
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
            break;
        request.append(buffer, static_cast<std::size_t>(n));
    }
    return request;
}

inline std::string request_method(const std::string& request) {
    const std::size_t end = request.find(' ');
    return end == std::string::npos ? std::string() : request.substr(0, end);
}

inline std::string request_target(const std::string& request) {
    const std::size_t first = request.find(' ');
    if (first == std::string::npos)
        return "/";
    const std::size_t second = request.find(' ', first + 1);
    if (second == std::string::npos)
        return "/";
    return request.substr(first + 1, second - first - 1);
}

inline std::string request_path(const std::string& target) {
    const std::size_t query = target.find('?');
    return query == std::string::npos ? target : target.substr(0, query);
}

inline std::string request_body(const std::string& request) {
    const std::size_t header_end = request.find("\r\n\r\n");
    return header_end == std::string::npos ? std::string() : request.substr(header_end + 4);
}

inline std::map<std::string, std::string> request_headers(const std::string& request) {
    std::map<std::string, std::string> out;
    const std::size_t header_end = request.find("\r\n\r\n");
    const std::string block = request.substr(0, header_end);
    std::size_t start = block.find("\r\n");
    if (start == std::string::npos)
        return out;
    start += 2;
    while (start < block.size()) {
        const std::size_t end = block.find("\r\n", start);
        const std::string line =
            block.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos)
            out[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        if (end == std::string::npos)
            break;
        start = end + 2;
    }
    return out;
}

inline std::string status_text(int status) {
    switch (status) {
    case 200:
        return "200 OK";
    case 400:
        return "400 Bad Request";
    case 404:
        return "404 Not Found";
    case 429:
        return "429 Too Many Requests";
    case 500:
        return "500 Internal Server Error";
    default:
        return std::to_string(status) + " Error";
    }
}

inline void send_all(int fd, const std::string& body, const std::string& content_type, int status,
                     int retry_after_s = 0) {
    std::string response = "HTTP/1.1 " + status_text(status) + "\r\nContent-Type: " + content_type +
                           "\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n";
    if (retry_after_s > 0)
        response += "Retry-After: " + std::to_string(retry_after_s) + "\r\n";
    response += "\r\n" + body;
    std::size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t n = send(fd, response.c_str() + sent, response.size() - sent, 0);
        if (n <= 0)
            return;
        sent += static_cast<std::size_t>(n);
    }
}

} // namespace bench::http_wire

#endif // BENCH_HTTP_WIRE_H
