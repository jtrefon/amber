#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>

#include "agent/archive_util.h"
#include "tests/test_util.h"

namespace {

namespace fs = std::filesystem;

std::string unique_dir(const char* tag) {
    static int sequence = 0;
    const fs::path path =
        fs::temp_directory_path() / ("amber-archive-" + std::string(tag) + "-" +
                                     std::to_string(getpid()) + "-" + std::to_string(sequence++));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path.string();
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

// One HTTP response on loopback, then the listener closes. The poll bounds the
// wait so a client that never connects cannot hang the test teardown.
class OneShotReply {
public:
    OneShotReply(int status, std::string body) : status_(status), body_(std::move(body)) {}
    ~OneShotReply() {
        if (worker_.joinable())
            worker_.join();
        if (fd_ >= 0)
            ::close(fd_);
    }

    bool start() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
            return false;
        socklen_t len = sizeof addr;
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            return false;
        port_ = ntohs(addr.sin_port);
        if (::listen(fd_, 1) != 0)
            return false;
        worker_ = std::thread([this] { serve(); });
        return true;
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/x"; }

private:
    void serve() {
        pollfd pending{fd_, POLLIN, 0};
        if (::poll(&pending, 1, 5000) <= 0)
            return;
        const int client = ::accept(fd_, nullptr, nullptr);
        if (client < 0)
            return;
        char request[1024];
        const ssize_t received = ::read(client, request, sizeof request);
        (void)received;
        const std::string response = "HTTP/1.1 " + std::to_string(status_) +
                                     " X\r\nContent-Length: " + std::to_string(body_.size()) +
                                     "\r\nConnection: close\r\n\r\n" + body_;
        const ssize_t sent = ::write(client, response.data(), response.size());
        (void)sent;
        ::close(client);
    }

    int fd_ = -1;
    int port_ = 0;
    int status_;
    std::string body_;
    std::thread worker_;
};

} // namespace

TEST(archive_util_reads_a_local_file) {
    const std::string root = unique_dir("file");
    const std::string path = root + "/body.txt";
    write_text(path, "payload\n");
    std::string err;
    ASSERT_EQ(agent::fetch_bytes(path, err), std::string("payload\n"));
    ASSERT_TRUE(err.empty());
    fs::remove_all(root);
}

TEST(archive_util_reports_a_missing_file) {
    const std::string root = unique_dir("missing");
    std::string err;
    ASSERT_EQ(agent::fetch_bytes(root + "/absent.txt", err), std::string());
    ASSERT(err.find("cannot read file") != std::string::npos);
    fs::remove_all(root);
}

TEST(archive_util_fetches_http_and_reports_http_errors) {
    OneShotReply ok(200, "hello");
    ASSERT_TRUE(ok.start());
    std::string err;
    ASSERT_EQ(agent::fetch_bytes(ok.url(), err), std::string("hello"));
    ASSERT_TRUE(err.empty());

    OneShotReply missing(404, "not here");
    ASSERT_TRUE(missing.start());
    ASSERT_EQ(agent::fetch_bytes(missing.url(), err), std::string());
    ASSERT(err.find("HTTP 404") != std::string::npos);
}

TEST(archive_util_lists_and_unpacks_paths_with_spaces_and_quotes) {
    const std::string root = unique_dir("tar");
    write_text(root + "/a.txt", "payload\n");
    const std::string archive = root + "/pack ' edge.tar.gz";
    const std::string dest = root + "/out ' dir";
    fs::create_directories(dest);

    const std::string create = "tar -czf \"" + archive + "\" -C \"" + root + "\" a.txt";
    ASSERT_EQ(std::system(create.c_str()), 0);

    const std::string listing = agent::list_tar_gz(archive);
    ASSERT(listing.find("a.txt") != std::string::npos);

    (void)agent::unpack_tar_gz(archive, dest);
    ASSERT(fs::exists(dest + "/a.txt"));
    std::ifstream in(dest + "/a.txt");
    std::string body;
    std::getline(in, body);
    ASSERT_EQ(body, std::string("payload"));
    fs::remove_all(root);
}
