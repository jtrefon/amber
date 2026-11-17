#include "bench/client_filters.h"

#include <set>
#include <stdexcept>
#include <utility>

namespace bench {

namespace {

std::uint64_t fnv1a(std::uint64_t hash, const std::string& text) {
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t prefix_hash(const std::vector<agent::Message>& messages,
                          const std::vector<std::shared_ptr<agent::Tool>>& tools) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const agent::Message& message : messages) {
        if (message.role == "system")
            hash = fnv1a(hash, "system:" + message.content + "\n");
    }
    for (const auto& tool : tools)
        hash = fnv1a(hash, "tool:" + tool->name() + tool->parameters_schema().dump() + "\n");
    return hash;
}

std::set<std::string> tool_names(const std::vector<std::shared_ptr<agent::Tool>>& tools) {
    std::set<std::string> names;
    for (const auto& tool : tools)
        names.insert(tool->name());
    return names;
}

void require_tool(const std::set<std::string>& names, const agent::json& wanted, bool present) {
    for (const auto& item : wanted) {
        const std::string name = item.get<std::string>();
        const bool found = names.count(name) != 0;
        if (found != present) {
            const std::string why =
                present ? "missing required tool: " : "forbidden tool present: ";
            std::string message = "request contract: ";
            message += why;
            message += name;
            throw std::runtime_error(message);
        }
    }
}

} // namespace

ContractClient::ContractClient(std::unique_ptr<agent::LLMClient> inner, agent::json contract,
                               std::string* violation)
    : inner_(std::move(inner)), contract_(std::move(contract)), violation_(violation) {}

ContractClient::~ContractClient() = default;

agent::ServerInfo ContractClient::probe_server() const {
    return inner_->probe_server();
}

int ContractClient::learned_context_size() const {
    return inner_->learned_context_size();
}

std::vector<std::string> ContractClient::take_notices() {
    return inner_->take_notices();
}

void ContractClient::set_session_id(const std::string& id) {
    inner_->set_session_id(id);
}

void ContractClient::check(const std::vector<agent::Message>& messages,
                           const std::vector<std::shared_ptr<agent::Tool>>& tools) const {
    const bool no_system_after = contract_.value("no_system_after_first", false);
    for (const agent::Message& message : messages) {
        if (no_system_after && message.role == "system" && seen_non_system_)
            throw std::runtime_error("request contract: system message after the first block");
        if (message.role != "system")
            seen_non_system_ = true;
    }
    if (contract_.value("system_prefix_stable", false)) {
        const std::uint64_t current = prefix_hash(messages, tools);
        if (!have_prefix_) {
            have_prefix_ = true;
            prefix_hash_ = current;
        } else if (current != prefix_hash_) {
            throw std::runtime_error("request contract: system prefix changed");
        }
    }
    const std::set<std::string> names = tool_names(tools);
    if (contract_.contains("tools_include"))
        require_tool(names, contract_["tools_include"], true);
    if (contract_.contains("tools_exclude"))
        require_tool(names, contract_["tools_exclude"], false);
}

void ContractClient::check_or_record(const std::vector<agent::Message>& messages,
                                     const std::vector<std::shared_ptr<agent::Tool>>& tools) const {
    try {
        check(messages, tools);
    } catch (const std::runtime_error& error) {
        if (!violation_)
            throw;
        *violation_ = error.what();
    }
}

agent::Message ContractClient::chat(const std::vector<agent::Message>& messages,
                                    const std::vector<std::shared_ptr<agent::Tool>>& tools,
                                    agent::Stats* stats) {
    check_or_record(messages, tools);
    return inner_->chat(messages, tools, stats);
}

agent::Message
ContractClient::chat_stream(const std::vector<agent::Message>& messages,
                            const std::vector<std::shared_ptr<agent::Tool>>& tools,
                            const std::function<void(const agent::StreamChunk&)>& on_chunk,
                            agent::Stats* stats) {
    check_or_record(messages, tools);
    return inner_->chat_stream(messages, tools, on_chunk, stats);
}

FaultClient::FaultClient(std::unique_ptr<agent::LLMClient> inner,
                         const std::vector<agent::json>& faults)
    : inner_(std::move(inner)) {
    for (const auto& item : faults) {
        Fault fault;
        fault.at_request = item.value("at_request", 0);
        fault.kind = item.value("kind", std::string());
        fault.after_chunks = item.value("after_chunks", 1);
        faults_.push_back(std::move(fault));
    }
}

FaultClient::~FaultClient() = default;

agent::ServerInfo FaultClient::probe_server() const {
    return inner_->probe_server();
}

int FaultClient::learned_context_size() const {
    return inner_->learned_context_size();
}

std::vector<std::string> FaultClient::take_notices() {
    return inner_->take_notices();
}

void FaultClient::set_session_id(const std::string& id) {
    inner_->set_session_id(id);
}

const FaultClient::Fault* FaultClient::fault_for(int request) const {
    for (const Fault& fault : faults_)
        if (fault.at_request == request)
            return &fault;
    return nullptr;
}

agent::Message FaultClient::empty_message() {
    agent::Message message;
    message.role = "assistant";
    return message;
}

void FaultClient::throw_fault(const Fault& fault) {
    if (fault.kind == "http_429")
        throw agent::ApiError(429, true, "scripted 429");
    if (fault.kind == "http_500")
        throw agent::ApiError(500, true, "scripted 500");
    if (fault.kind == "disconnect")
        throw agent::ApiError(0, true, "scripted disconnect");
    if (fault.kind == "malformed_json")
        throw agent::ApiError(0, false, "scripted malformed JSON");
    if (fault.kind == "stream_drop")
        throw agent::ApiError(503, true, "scripted mid-stream dropout");
    throw agent::ApiError(500, true, "scripted fault");
}

agent::Message FaultClient::chat(const std::vector<agent::Message>& messages,
                                 const std::vector<std::shared_ptr<agent::Tool>>& tools,
                                 agent::Stats* stats) {
    ++calls_;
    const Fault* fault = fault_for(calls_);
    if (fault && fault->kind == "empty_reply")
        return empty_message();
    if (fault)
        throw_fault(*fault);
    return inner_->chat(messages, tools, stats);
}

agent::Message
FaultClient::chat_stream(const std::vector<agent::Message>& messages,
                         const std::vector<std::shared_ptr<agent::Tool>>& tools,
                         const std::function<void(const agent::StreamChunk&)>& on_chunk,
                         agent::Stats* stats) {
    ++calls_;
    const Fault* fault = fault_for(calls_);
    if (fault && fault->kind == "empty_reply")
        return empty_message();
    if (fault && fault->kind == "stream_drop") {
        for (int i = 0; i < fault->after_chunks && on_chunk; ++i) {
            agent::StreamChunk chunk;
            chunk.delta = "x";
            on_chunk(chunk);
        }
        throw agent::ApiError(503, true, "scripted mid-stream dropout");
    }
    if (fault)
        throw_fault(*fault);
    return inner_->chat_stream(messages, tools, on_chunk, stats);
}

} // namespace bench
