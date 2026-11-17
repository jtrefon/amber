#ifndef BENCH_CLIENT_FILTERS_H
#define BENCH_CLIENT_FILTERS_H

// LLMClient decorators for scenario-level assertions. ContractClient checks
// the request shape before delegating; FaultClient injects a deterministic
// failure at a chosen request index.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agent/llm.h"
#include "agent/tool.h"

namespace bench {

class ContractClient : public agent::LLMClient {
public:
    ContractClient(std::unique_ptr<agent::LLMClient> inner, agent::json contract,
                   std::string* violation = nullptr);
    ~ContractClient() override;

    agent::ServerInfo probe_server() const override;
    agent::Message chat(const std::vector<agent::Message>& messages,
                        const std::vector<std::shared_ptr<agent::Tool>>& tools,
                        agent::Stats* stats = nullptr) override;
    agent::Message chat_stream(const std::vector<agent::Message>& messages,
                               const std::vector<std::shared_ptr<agent::Tool>>& tools,
                               const std::function<void(const agent::StreamChunk&)>& on_chunk,
                               agent::Stats* stats = nullptr) override;
    int learned_context_size() const override;
    std::vector<std::string> take_notices() override;
    void set_session_id(const std::string& id) override;

private:
    void check(const std::vector<agent::Message>& messages,
               const std::vector<std::shared_ptr<agent::Tool>>& tools) const;
    void check_or_record(const std::vector<agent::Message>& messages,
                         const std::vector<std::shared_ptr<agent::Tool>>& tools) const;

    std::unique_ptr<agent::LLMClient> inner_;
    agent::json contract_;
    std::string* violation_ = nullptr;
    mutable bool have_prefix_ = false;
    mutable std::uint64_t prefix_hash_ = 0;
    mutable bool seen_non_system_ = false;
};

class FaultClient : public agent::LLMClient {
public:
    FaultClient(std::unique_ptr<agent::LLMClient> inner, const std::vector<agent::json>& faults);
    ~FaultClient() override;

    agent::ServerInfo probe_server() const override;
    agent::Message chat(const std::vector<agent::Message>& messages,
                        const std::vector<std::shared_ptr<agent::Tool>>& tools,
                        agent::Stats* stats = nullptr) override;
    agent::Message chat_stream(const std::vector<agent::Message>& messages,
                               const std::vector<std::shared_ptr<agent::Tool>>& tools,
                               const std::function<void(const agent::StreamChunk&)>& on_chunk,
                               agent::Stats* stats = nullptr) override;
    int learned_context_size() const override;
    std::vector<std::string> take_notices() override;
    void set_session_id(const std::string& id) override;

private:
    struct Fault {
        int at_request = 0;
        std::string kind;
        int after_chunks = 1;
    };

    const Fault* fault_for(int request) const;
    static void throw_fault(const Fault& fault);
    static agent::Message empty_message();

    std::unique_ptr<agent::LLMClient> inner_;
    std::vector<Fault> faults_;
    int calls_ = 0;
};

} // namespace bench

#endif // BENCH_CLIENT_FILTERS_H
