
#ifndef AGENT_FAKE_LLM_H
#define AGENT_FAKE_LLM_H

// In-memory LLMClient for hermetic agent-loop tests (docs/spec/llm-client/
// agent-loop-reliability.md). Serves scripted replies in order and records
// every request for assertions. Never touches the network.

#include <deque>
#include <string>
#include <vector>

#include "agent/llm.h"

namespace agent_test {

// One scripted response (or failure) served by FakeLLMClient.
struct FakeReply {
    std::string content;               // assistant text (may be empty)
    json tool_calls = json::array();   // assistant tool_calls
    std::string error;                 // non-empty -> throw instead of replying
    bool retryable = true;             // ApiError.retryable when throwing
    long prompt_tokens = 0;            // stats.prompt_tokens for this reply
    long completion_tokens = 0;
};

class FakeLLMClient : public agent::LLMClient {
public:
    std::deque<FakeReply> script;
    std::vector<std::vector<agent::Message>> requests;
    std::vector<size_t> tool_counts;
    int chat_calls = 0;

    agent::ServerInfo probe_server() const override {
        agent::ServerInfo info;
        info.ok = true;
        info.model = "fake";
        info.context_size = 0;
        return info;
    }

    agent::Message chat(const std::vector<agent::Message>& messages,
                        const std::vector<agent::Tool*>& tools,
                        agent::Stats* stats = nullptr) override {
        ++chat_calls;
        requests.push_back(messages);
        tool_counts.push_back(tools.size());
        FakeReply r;
        if (!script.empty()) {
            r = std::move(script.front());
            script.pop_front();
        }
        if (stats) {
            stats->prompt_tokens = r.prompt_tokens;
            stats->completion_tokens = r.completion_tokens;
            stats->valid = true;
        }
        if (!r.error.empty())
            throw agent::ApiError(r.retryable ? 503 : 401, r.retryable,
                                  r.error);
        agent::Message m;
        m.role = "assistant";
        m.content = r.content;
        m.tool_calls = r.tool_calls;
        return m;
    }

    agent::Message chat_stream(
        const std::vector<agent::Message>& messages,
        const std::vector<agent::Tool*>& tools,
        const std::function<void(const agent::StreamChunk&)>& on_chunk,
        agent::Stats* stats = nullptr) override {
        (void)on_chunk;
        return chat(messages, tools, stats);
    }
};

// Build an assistant message carrying one tool call.
inline agent::Message tool_call_msg(const std::string& fn, const json& args,
                                    const std::string& id = "call_1") {
    agent::Message m;
    m.role = "assistant";
    m.tool_calls = json::array(
        {{{"id", id},
          {"type", "function"},
          {"function", {{"name", fn}, {"arguments", args.dump()}}}}});
    return m;
}

} // namespace agent_test

#endif // AGENT_FAKE_LLM_H
