#include "bench/policy_client.h"

#include <thread>
#include <utility>

namespace bench {

namespace {

std::string tool_status(const std::string& envelope) {
    const std::string key = "status=";
    const std::size_t at = envelope.find(key);
    if (at == std::string::npos)
        return {};
    const std::size_t start = at + key.size();
    const std::size_t end = envelope.find_first_of(" \n]", start);
    return envelope.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

agent::Message make_message(const PolicyReply& reply) {
    agent::Message message;
    message.role = "assistant";
    message.content = reply.content;
    message.tool_calls = reply.tool_calls;
    message.reasoning = reply.reasoning;
    return message;
}

void fill_stats(const PolicyReply& reply, agent::Stats* stats) {
    if (!stats)
        return;
    stats->latency_ms = static_cast<double>(reply.latency_ms);
    stats->tps = reply.latency_ms > 0 && reply.completion_tokens > 0
                     ? 1000.0 * static_cast<double>(reply.completion_tokens) /
                           static_cast<double>(reply.latency_ms)
                     : -1.0;
    stats->prompt_tokens = reply.prompt_tokens;
    stats->completion_tokens = reply.completion_tokens;
    stats->valid = true;
}

} // namespace

PolicyClient::PolicyClient(std::unique_ptr<Policy> policy) : policy_(std::move(policy)) {}

agent::ServerInfo PolicyClient::probe_server() const {
    agent::ServerInfo info;
    info.ok = true;
    info.model = "policy";
    info.context_size = 0;
    return info;
}

PolicyReply PolicyClient::next(const std::vector<agent::Message>& messages,
                               const std::vector<std::shared_ptr<agent::Tool>>& tools) {
    PolicyContext context;
    context.step = chat_calls;
    context.messages = &messages;
    context.tools = &tools;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role != "tool")
            continue;
        context.last_tool_name = it->name;
        context.last_tool_result = it->content;
        context.last_tool_status = tool_status(it->content);
        break;
    }
    return policy_->next(context);
}

agent::Message PolicyClient::chat(const std::vector<agent::Message>& messages,
                                  const std::vector<std::shared_ptr<agent::Tool>>& tools,
                                  agent::Stats* stats) {
    ++chat_calls;
    tool_counts.push_back(tools.size());
    const PolicyReply reply = next(messages, tools);
    if (reply.latency_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(reply.latency_ms));
    fill_stats(reply, stats);
    if (!reply.error.empty())
        throw agent::ApiError(reply.retryable ? 503 : 401, reply.retryable, reply.error);
    return make_message(reply);
}

agent::Message
PolicyClient::chat_stream(const std::vector<agent::Message>& messages,
                          const std::vector<std::shared_ptr<agent::Tool>>& tools,
                          const std::function<void(const agent::StreamChunk&)>& on_chunk,
                          agent::Stats* stats) {
    ++chat_calls;
    tool_counts.push_back(tools.size());
    const PolicyReply reply = next(messages, tools);
    if (reply.latency_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(reply.latency_ms));
    if (reply.drop_after_chunks > 0) {
        for (int i = 0; i < reply.drop_after_chunks && on_chunk; ++i) {
            agent::StreamChunk chunk;
            chunk.delta = reply.chunk_delta;
            on_chunk(chunk);
        }
        throw agent::ApiError(reply.retryable ? 503 : 401, reply.retryable,
                              "simulated mid-stream dropout");
    }
    if (on_chunk && !reply.content.empty()) {
        agent::StreamChunk chunk;
        chunk.delta = reply.content;
        on_chunk(chunk);
    }
    fill_stats(reply, stats);
    if (!reply.error.empty())
        throw agent::ApiError(reply.retryable ? 503 : 401, reply.retryable, reply.error);
    return make_message(reply);
}

} // namespace bench
