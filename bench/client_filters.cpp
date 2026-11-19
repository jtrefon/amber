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
    if (fault.kind == "http_400")
        throw agent::ApiError(400, false, "scripted 400");
    if (fault.kind == "timeout")
        throw agent::ApiError(0, true, "scripted timeout");
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

namespace {

bool log_has_tool(const std::vector<std::string>& names, const std::string& wanted) {
    return std::find(names.begin(), names.end(), wanted) != names.end();
}

void log_tool_error(const char* key, const char* why, const std::string& name, int index,
                    std::string& err) {
    std::string message = "request " + std::to_string(index) + ": ";
    message += key;
    message += " ";
    message += why;
    message += " '";
    message += name;
    message += "'";
    err = message;
}

bool check_log_tools(const agent::json& contract, const EndpointRequest& request, int index,
                     std::string& err) {
    const std::vector<std::string> names = endpoint_tool_names(request.json);
    if (contract.contains("tools_include")) {
        for (const auto& item : contract["tools_include"]) {
            const std::string wanted = item.get<std::string>();
            if (!log_has_tool(names, wanted)) {
                log_tool_error("tools_include", "is missing", wanted, index, err);
                return false;
            }
        }
    }
    if (contract.contains("tools_exclude")) {
        for (const auto& item : contract["tools_exclude"]) {
            const std::string unwanted = item.get<std::string>();
            if (log_has_tool(names, unwanted)) {
                log_tool_error("tools_exclude", "was offered", unwanted, index, err);
                return false;
            }
        }
    }
    return true;
}

bool check_log_system_order(const EndpointRequest& request, int index, std::string& err) {
    bool seen_non_system = false;
    for (const agent::Message& message : request.messages) {
        if (message.role != "system") {
            seen_non_system = true;
            continue;
        }
        if (seen_non_system) {
            err = "request " + std::to_string(index) + ": system message after the first block";
            return false;
        }
    }
    return true;
}

std::string log_prefix_key(const EndpointRequest& request) {
    std::string key = endpoint_system_text(request.json);
    for (const std::string& name : endpoint_tool_names(request.json)) {
        key += '\n';
        key += name;
    }
    return key;
}

} // namespace

bool check_request_log(const agent::json& contract, const std::vector<EndpointRequest>& requests,
                       std::string& err) {
    err.clear();
    if (contract.empty())
        return true;
    const bool stable = contract.value("system_prefix_stable", false);
    const bool ordered = contract.value("no_system_after_first", false);
    std::string prefix;
    bool have_prefix = false;
    int index = 0;
    for (const EndpointRequest& request : requests) {
        if (!endpoint_is_chat_path(request.path))
            continue;
        ++index;
        if (contract.contains("path") && contract["path"].is_string() &&
            request.path != contract["path"].get<std::string>()) {
            err = "request " + std::to_string(index) + ": path expected '" +
                  contract["path"].get<std::string>() + "', got '" + request.path + "'";
            return false;
        }
        if (ordered && !check_log_system_order(request, index, err))
            return false;
        if (!check_log_tools(contract, request, index, err))
            return false;
        if (!stable)
            continue;
        const std::string key = log_prefix_key(request);
        if (!have_prefix) {
            have_prefix = true;
            prefix = key;
        } else if (key != prefix) {
            err = "request " + std::to_string(index) + ": system prefix changed";
            return false;
        }
    }
    return true;
}

} // namespace bench
