
#ifndef BENCH_FAKE_H
#define BENCH_FAKE_H

// Hermetic LLM client for benchmark runs: scripted replies with configurable
// latency, streaming deltas with mid-stream dropout, retryable/non-retryable
// errors, and per-reply token stats. Deterministic — no network.

#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "agent/llm.h"

namespace bench {

struct BenchReply {
    std::string content;
    agent::json tool_calls = agent::json::array();
    std::string error;                  // non-empty -> throw ApiError
    bool retryable = true;
    long latency_ms = 0;                // simulated time-to-first-byte
    int drop_after_chunks = 0;          // >0 -> throw after N stream chunks
    std::string chunk_delta = "x";      // text per streamed chunk
    long prompt_tokens = 0;
    long completion_tokens = 0;
};

class FakeClient : public agent::LLMClient {
public:
    std::deque<BenchReply> script;
    std::vector<size_t> tool_counts;
    int chat_calls = 0;

    agent::ServerInfo probe_server() const override;

    agent::Message chat(const std::vector<agent::Message>& messages,
                        const std::vector<std::shared_ptr<agent::Tool>>& tools,
                        agent::Stats* stats = nullptr) override;

    agent::Message chat_stream(
        const std::vector<agent::Message>& messages,
        const std::vector<std::shared_ptr<agent::Tool>>& tools,
        const std::function<void(const agent::StreamChunk&)>& on_chunk,
        agent::Stats* stats = nullptr) override;
};

} // namespace bench

#endif // BENCH_FAKE_H
