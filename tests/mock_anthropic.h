#ifndef AMBER_TESTS_MOCK_ANTHROPIC_H
#define AMBER_TESTS_MOCK_ANTHROPIC_H

// A local Anthropic Messages endpoint that emulates the provider's prompt
// cache, so a green test means "a real provider would have hit".
//
// The rules it reproduces are the ones a client can get wrong without any
// error: caching is opt-in per block (`cache_control`), the prefix is
// tools -> system -> messages, a prefix under the model's minimum is silently
// not cached (0/0, no warning), and the response reports the split as
// `cache_read_input_tokens` / `cache_creation_input_tokens` / `input_tokens`.
//
// It records the request bodies and headers, which is the only way to assert
// what the wire actually carried: the dialect is otherwise invisible.

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent::test {

struct MockRequest {
    std::string path;
    std::map<std::string, std::string> headers; // lower-cased names
    std::string body;
};

// What the provider reported for the most recent request. The split is the
// contract: read + write + input is the whole prompt, and the client must
// present that total as the context it consumed.
struct MockUsage {
    long input = 0;
    long read = 0;
    long write = 0;
};

class MockAnthropic {
public:
    ~MockAnthropic();
    MockAnthropic() = default;
    MockAnthropic(const MockAnthropic&) = delete;
    MockAnthropic& operator=(const MockAnthropic&) = delete;

    bool start();
    void stop();

    // "http://127.0.0.1:<port>", the shape Config::api_base expects.
    std::string base() const;

    // The provider's documented minimum cacheable prefix. Below it the real
    // API caches nothing and returns no error.
    void set_min_cacheable_tokens(long tokens);

    std::vector<MockRequest> requests();
    MockUsage last_usage();

private:
    // One written prefix: how many tokens it covers and when it expires. The
    // real cache is refreshed on every read, which is why a read extends the
    // lifetime without a new write.
    struct CacheEntry {
        long tokens = 0;
        std::chrono::steady_clock::time_point expires;
    };

    void serve();
    void handle(int conn);
    std::string message_json(const std::string& body, bool stream);

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex mtx_;
    std::vector<MockRequest> requests_;
    MockUsage last_usage_;
    std::map<std::string, CacheEntry> cache_;
    long min_tokens_ = 1024;
};

} // namespace agent::test

#endif // AMBER_TESTS_MOCK_ANTHROPIC_H
