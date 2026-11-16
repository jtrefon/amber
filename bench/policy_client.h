#ifndef BENCH_POLICY_CLIENT_H
#define BENCH_POLICY_CLIENT_H

// An LLMClient backed by a deterministic Policy. It gives the policy the
// conversation and the last tool result, so the reply can depend on what the
// harness actually observed.

#include <memory>
#include <string>
#include <vector>

#include "bench/policy.h"

namespace bench {

class PolicyClient : public agent::LLMClient {
public:
    explicit PolicyClient(std::unique_ptr<Policy> policy);

    agent::ServerInfo probe_server() const override;
    agent::Message chat(const std::vector<agent::Message>& messages,
                        const std::vector<std::shared_ptr<agent::Tool>>& tools,
                        agent::Stats* stats = nullptr) override;
    agent::Message chat_stream(const std::vector<agent::Message>& messages,
                               const std::vector<std::shared_ptr<agent::Tool>>& tools,
                               const std::function<void(const agent::StreamChunk&)>& on_chunk,
                               agent::Stats* stats = nullptr) override;

    int chat_calls = 0;
    std::vector<std::size_t> tool_counts;

private:
    PolicyReply next(const std::vector<agent::Message>& messages,
                     const std::vector<std::shared_ptr<agent::Tool>>& tools);
    std::unique_ptr<Policy> policy_;
};

} // namespace bench

#endif // BENCH_POLICY_CLIENT_H
