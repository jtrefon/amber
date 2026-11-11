#ifndef AMBER_PLUGINS_CACHE_REUSE_H
#define AMBER_PLUGINS_CACHE_REUSE_H

// The pure model behind the cache gauge. It is deliberately independent of the
// plugin lifecycle and the terminal: tests drive it with injected timestamps,
// and the plugin only adapts events and renders the readout.
//
// The number is a rolling window, not a session average: the last N supported
// main-conversation calls inside the provider cache TTL. A call older than the
// TTL is irrelevant to "what is cached now"; a burst from yesterday must not
// colour today's gauge.

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace agent::plugins::cache {

enum class Metric { Tokens, Calls };

enum class State {
    NoData,      // no completed supported request yet
    Unsupported, // the provider reports no cache counters
    Cold,        // the newest sample is older than the cache TTL
    Warm,        // cache writes happened, but no read yet
    Reuse,       // at least one cache read in the window
};

struct Sample {
    std::chrono::steady_clock::time_point at;
    long input = 0;
    long read = 0;
    long write = 0;
};

// One completed main-conversation exchange. The aggregate keeps `record()`'s
// parameter count inside the project's wide-function cap.
struct RecordInput {
    std::chrono::steady_clock::time_point at;
    long input = 0;
    long read = 0;
    long write = 0;
    std::string model;
    std::string provider;
    std::string miss_reason;
};

struct Readout {
    State state = State::NoData;
    int percent = 0; // floor of the primary metric, 0..100
    bool sub_one = false;
    int call_hit_percent = 0;
    bool call_hit_sub_one = false;
    int calls = 0;
    int hit_calls = 0;
    long input_tokens = 0;
    long read_tokens = 0;
    long write_tokens = 0;
    long long age_seconds = 0;
    std::string model;
    std::string provider;
    std::string miss_reason;
};

class CacheWindow {
public:
    enum class Record {
        Accepted,    // a supported sample entered the window
        Unsupported, // cache counters were absent; the window is not usable
        Ignored,     // no usable prompt count; leave the window alone
    };

    // Add one completed main-conversation exchange. A model/provider change
    // resets the window first, so two vendors (or two models with different
    // cache behaviour) never share a percentage.
    Record record(const RecordInput& input, std::size_t max_samples);

    void reset();

    // Pure read. `ttl` is the provider cache lifetime; `max_samples` is the
    // rolling-window cap. No I/O, no sleeping, no session totals.
    Readout view(std::chrono::steady_clock::time_point now, Metric metric, std::size_t max_samples,
                 std::chrono::seconds ttl) const;

private:
    std::vector<const Sample*> recent_samples(std::chrono::steady_clock::time_point now,
                                              std::size_t max_samples,
                                              std::chrono::seconds ttl) const;
    static Readout summarize(const std::vector<const Sample*>& recent, Metric metric);

    std::deque<Sample> samples_;
    std::string model_;
    std::string provider_;
    std::string miss_reason_;
    bool unsupported_ = false;
};

} // namespace agent::plugins::cache

#endif // AMBER_PLUGINS_CACHE_REUSE_H
