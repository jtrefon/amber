
// RED: stub — fake LLM client not implemented yet (empty replies).

#include "bench/fake.h"

namespace bench {

agent::ServerInfo FakeClient::probe_server() const {
    return agent::ServerInfo{};
}

agent::Message FakeClient::chat(const std::vector<agent::Message>&,
                                const std::vector<agent::Tool*>&,
                                agent::Stats*) {
    ++chat_calls;
    return agent::Message{};
}

agent::Message FakeClient::chat_stream(
    const std::vector<agent::Message>&, const std::vector<agent::Tool*>&,
    const std::function<void(const agent::StreamChunk&)>&, agent::Stats*) {
    ++chat_calls;
    return agent::Message{};
}

agent::Message FakeClient::serve(BenchReply, agent::Stats*) const {
    return agent::Message{};
}

} // namespace bench
