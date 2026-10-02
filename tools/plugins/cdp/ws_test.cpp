// Hermetic test for the WebSocket client: a local echo server (raw TCP +
// minimal RFC 6455 frame handling) verifies handshake, send, receive, and
// masking round-trip without any external service.
#include "ws_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include "tests/minitest.h"

namespace {

int server_fd = -1;
int client_fd = -1;
std::atomic<bool> server_done{false};
std::atomic<bool> listening{false};
std::string echo_payload;

// Bind and listen on an ephemeral loopback port (recorded for
// ephemeral_port()). False on failure.
bool bind_listener() {
    struct sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    auto* sa_ptr = reinterpret_cast<struct sockaddr*>(&sa);
    if (bind(server_fd, sa_ptr, sizeof sa) != 0)
        return false;
    socklen_t slen = sizeof sa;
    getsockname(server_fd, sa_ptr, &slen);
    listen(server_fd, 1);
    listening.store(true);
    return true;
}

// Read the HTTP handshake, then reply 101 (the accept value is not verified by
// the client).
bool complete_handshake() {
    std::string req;
    std::array<char, 1024> tmp{};
    while (req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(client_fd, tmp.data(), tmp.size(), 0);
        if (n <= 0)
            return false;
        req.append(tmp.data(), (size_t)n);
    }
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    (void)send(client_fd, resp.data(), resp.size(), 0);
    return true;
}

// Read one masked client frame, unmasking it into `payload`. False when the
// peer went away.
bool read_frame(std::string& payload, unsigned char& opcode) {
    std::array<unsigned char, 2> hdr{};
    if (recv(client_fd, hdr.data(), hdr.size(), 0) <= 0)
        return false;
    size_t len = hdr[1] & 0x7f;
    if (len == 126) {
        std::array<unsigned char, 2> ext{};
        if (recv(client_fd, ext.data(), ext.size(), 0) != 2)
            return false;
        len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
    }
    std::array<unsigned char, 4> mask{};
    if (recv(client_fd, mask.data(), mask.size(), 0) != 4)
        return false;
    payload.assign(len, '\0');
    if (recv(client_fd, payload.data(), len, 0) != static_cast<ssize_t>(len))
        return false;
    for (size_t i = 0; i < len; ++i)
        payload[i] ^= mask[i % 4];
    opcode = hdr[0] & 0x0f;
    return true;
}

void server_main() {
    if (!bind_listener())
        return;
    client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0)
        return;
    if (!complete_handshake())
        return;

    // Echo each masked text frame back as an unmasked server frame.
    while (!server_done.load()) {
        std::string payload;
        unsigned char opcode = 0;
        if (!read_frame(payload, opcode))
            return;
        if (opcode == 0x8)
            return; // close
        if (opcode != 0x1)
            continue;
        echo_payload = payload;
        std::string frame;
        frame += static_cast<char>(0x81);
        frame += static_cast<char>(payload.size());
        frame += payload;
        (void)send(client_fd, frame.data(), frame.size(), 0);
    }
}

int ephemeral_port() {
    struct sockaddr_in sa {};
    socklen_t slen = sizeof sa;
    auto* sa_ptr = reinterpret_cast<struct sockaddr*>(&sa);
    getsockname(server_fd, sa_ptr, &slen);
    return ntohs(sa.sin_port);
}

} // namespace

int main() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(server_fd >= 0);
    std::thread server(server_main);
    while (!listening.load())
        usleep(2000);
    ASSERT(listening.load());

    cdp::WsClient ws;
    std::string err;
    std::string url = "ws://127.0.0.1:" + std::to_string(ephemeral_port()) + "/echo";
    ASSERT(ws.connect(url, err));
    ASSERT(ws.send_text("hello ws"));
    std::string reply;
    ASSERT(ws.recv_text(reply, 3000));
    ASSERT_EQ(reply, "hello ws");

    server_done.store(true);
    ws.close();
    server.join();
    ::close(client_fd);
    ::close(server_fd);

    if (failed)
        std::cerr << failed << " FAILED\n";
    std::cout << (failed ? "FAILED" : "ALL PASSED") << " (0 failures)\n";
    return failed ? 1 : 0;
}
