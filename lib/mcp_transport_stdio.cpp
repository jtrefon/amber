
#include "agent/mcp_transport_stdio.h"
#include "agent/process.h"

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace agent {

namespace {

constexpr size_t kStderrTailCap = static_cast<size_t>(64) * 1024;
constexpr int kEofGraceMs = 3000;
constexpr int kSigtermGraceMs = 3000;
constexpr int kSigkillGraceMs = 5000;

bool reap(pid_t pid, int ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (true) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno == ECHILD) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        usleep(10 * 1000);
    }
}

} // namespace

StdioTransport::StdioTransport(std::string command,
                               std::vector<std::string> args,
                               std::string cwd,
                               std::function<void(const McpMessage&)>
                                   on_server_message,
                               int request_timeout_ms)
    : McpTransport(std::move(on_server_message)),
      command_(std::move(command)),
      args_(std::move(args)),
      cwd_(std::move(cwd)),
      request_timeout_ms_(request_timeout_ms > 0 ? request_timeout_ms : 60000) {
    std::string err;
    pid_ = spawn_mcp_server(command_, args_, cwd_, stdin_fd_, stdout_fd_,
                            stderr_fd_, err);
    if (pid_ <= 0) {
        failure_ = "mcp spawn failed: " + err;
        return;
    }
    // Writes to a dead child's stdin would otherwise kill us via SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
    stdout_thread_ = std::thread(&StdioTransport::stdout_loop, this);
    stderr_thread_ = std::thread(&StdioTransport::stderr_loop, this);
}

StdioTransport::~StdioTransport() { StdioTransport::shutdown(); }

McpTransportResult StdioTransport::request(int id, const std::string& method,
                                           const json& params) {
    McpRequest req;
    req.id = id;
    req.method = method;
    req.params = params;
    if (!write_line(mcp_encode_request(req))) {
        McpTransportResult r;
        r.status = McpTransportStatus::TransportError;
        return r;
    }

    std::unique_lock<std::mutex> lk(mtx_);
    bool answered = cv_.wait_for(
        lk, std::chrono::milliseconds(request_timeout_ms_), [&] {
            return closed_.load() || pending_.count(id) > 0;
        });
    McpTransportResult r;
    if (!answered) {
        r.status = closed_.load() ? McpTransportStatus::TransportError
                                  : McpTransportStatus::Timeout;
        return r;
    }
    auto it = pending_.find(id);
    if (it == pending_.end()) {
        r.status = McpTransportStatus::TransportError;
        return r;
    }
    r.message = std::move(it->second);
    pending_.erase(it);
    return r;
}

bool StdioTransport::notify(const std::string& method, const json& params) {
    return write_line(mcp_encode_notification(method, params));
}

bool StdioTransport::respond(int id, const json& result) {
    return write_line(mcp_encode_response(id, result));
}

bool StdioTransport::respond_error(int id, const McpError& error) {
    return write_line(mcp_encode_error_response(id, error));
}

void StdioTransport::shutdown() {
    bool was_closed = closed_.exchange(true);
    if (!was_closed) {
        {
            std::scoped_lock lk(mtx_);
            failure_ = failure_.empty() ? "transport closed" : failure_;
        }
        cv_.notify_all();
        if (pid_ > 0) {
            if (stdin_fd_ >= 0) {
                close(stdin_fd_);
                stdin_fd_ = -1;
            }
            terminate_child();
            if (stdout_fd_ >= 0) {
                close(stdout_fd_);
                stdout_fd_ = -1;
            }
            if (stderr_fd_ >= 0) {
                close(stderr_fd_);
                stderr_fd_ = -1;
            }
        }
    }
    if (stdout_thread_.joinable()) stdout_thread_.join();
    if (stderr_thread_.joinable()) stderr_thread_.join();
}

std::string StdioTransport::failure_reason() const {
    std::scoped_lock lk(mtx_);
    if (failure_.empty()) return "";
    return failure_ + (stderr_tail_.empty() ? "" : "\n" + stderr_tail_);
}

void StdioTransport::stdout_loop() {
    std::string buf;
    char tmp[4096];
    while (!closed_.load()) {
        ssize_t n = read(stdout_fd_, tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            handle_line(line);
        }
        if (buf.size() > static_cast<size_t>(1024) * 1024) {
            fail("mcp server output was not newline-framed");
            return;
        }
    }
    if (!closed_.load()) fail("mcp server closed its output");
}

void StdioTransport::stderr_loop() {
    char tmp[4096];
    std::string tail;
    while (!closed_.load()) {
        ssize_t n = read(stderr_fd_, tmp, sizeof tmp);
        if (n <= 0) break;
        tail.append(tmp, static_cast<size_t>(n));
        if (tail.size() > kStderrTailCap)
            tail.erase(0, tail.size() - kStderrTailCap);
    }
    std::scoped_lock lk(mtx_);
    stderr_tail_ = tail;
}

void StdioTransport::handle_line(const std::string& line) {
    auto msg = mcp_decode_line(line);
    if (!msg) {
        fail("mcp server sent an invalid message");
        return;
    }
    if (msg->is_response()) {
        std::scoped_lock lk(mtx_);
        if (msg->id.has_value() && msg->id->is_number_integer())
            pending_[msg->id->get<int>()] = std::move(*msg);
        cv_.notify_all();
        return;
    }
    if (on_server_message_) on_server_message_(*msg);
}

bool StdioTransport::write_line(const std::string& line) {
    if (closed_.load() || stdin_fd_ < 0) return false;
    std::string out = line;
    out += "\n";
    size_t off = 0;
    while (off < out.size()) {
        ssize_t n = write(stdin_fd_, out.data() + off, out.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

void StdioTransport::terminate_child() const {
    if (pid_ <= 0) return;
    if (reap(pid_, kEofGraceMs)) return;
    kill(-pid_, SIGTERM);
    if (reap(pid_, kSigtermGraceMs)) return;
    kill(-pid_, SIGKILL);
    reap(pid_, kSigkillGraceMs);
}

void StdioTransport::fail(const std::string& reason) {
    {
        std::scoped_lock lk(mtx_);
        if (failure_.empty()) failure_ = reason;
    }
    cv_.notify_all();
    if (closed_.exchange(true)) return;
    if (pid_ > 0) terminate_child();
    if (stderr_fd_ >= 0) {
        close(stderr_fd_);
        stderr_fd_ = -1;
    }
}

} // namespace agent
