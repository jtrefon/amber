
#include "bench/fake.h"

#include <thread>

namespace bench {

namespace {

agent::Message make_message(const BenchReply& r) {
    agent::Message m;
    m.role = "assistant";
    m.content = r.content;
    m.tool_calls = r.tool_calls;
    return m;
}

void fill_stats(const BenchReply& r, agent::Stats* stats) {
    if (!stats) return;
    stats->latency_ms = static_cast<double>(r.latency_ms);
    stats->tps = r.latency_ms > 0 && r.completion_tokens > 0
                     ? 1000.0 * static_cast<double>(r.completion_tokens) /
                           static_cast<double>(r.latency_ms)
                     : -1.0;
    stats->prompt_tokens = r.prompt_tokens;
    stats->completion_tokens = r.completion_tokens;
    stats->valid = true;
}

} // namespace

agent::ServerInfo FakeClient::probe_server() const {
    agent::ServerInfo info;
    info.ok = true;
    info.model = "fake";
    info.context_size = 0;
    return info;
}

agent::Message FakeClient::chat(const std::vector<agent::Message>& messages,
                                const std::vector<std::shared_ptr<agent::Tool>>& tools,
                                agent::Stats* stats) {
    (void)messages;
    ++chat_calls;
    tool_counts.push_back(tools.size());
    BenchReply r;
    if (!script.empty()) {
        r = std::move(script.front());
        script.pop_front();
    }
    if (r.latency_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(r.latency_ms));
    fill_stats(r, stats);
    if (!r.error.empty())
        throw agent::ApiError(r.retryable ? 503 : 401, r.retryable, r.error);
    return make_message(r);
}

agent::Message FakeClient::chat_stream(
    const std::vector<agent::Message>& messages,
    const std::vector<std::shared_ptr<agent::Tool>>& tools,
    const std::function<void(const agent::StreamChunk&)>& on_chunk,
    agent::Stats* stats) {
    (void)messages;
    ++chat_calls;
    tool_counts.push_back(tools.size());
    BenchReply r;
    if (!script.empty()) {
        r = std::move(script.front());
        script.pop_front();
    }
    if (r.latency_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(r.latency_ms));
    if (r.drop_after_chunks > 0) {
        for (int i = 0; i < r.drop_after_chunks && on_chunk; ++i) {
            agent::StreamChunk c;
            c.delta = r.chunk_delta;
            on_chunk(c);
        }
        throw agent::ApiError(r.retryable ? 503 : 401, r.retryable,
                              "simulated mid-stream dropout");
    }
    if (on_chunk) {
        agent::StreamChunk c;
        c.delta = r.content;
        if (!r.content.empty()) on_chunk(c);
    }
    fill_stats(r, stats);
    if (!r.error.empty())
        throw agent::ApiError(r.retryable ? 503 : 401, r.retryable, r.error);
    return make_message(r);
}

} // namespace bench
