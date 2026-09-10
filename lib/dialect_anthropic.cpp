
#include "agent/dialect_anthropic.h"

#include "agent/agent_helpers.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace agent {

namespace {

constexpr const char* kApiVersion = "2023-06-01";
constexpr const char* kRoleUser = "user";
constexpr const char* kRoleAssistant = "assistant";

// Defensive string read: a non-string/absent field yields "", never a
// type_error — response bodies are untrusted input and an exception thrown
// inside a curl write callback cannot be recovered from.
std::string str_field(const json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// Parse a tool call's `arguments` (a JSON string on the internal model) into
// the object Anthropic expects for tool_use.input. Malformed arguments yield
// an empty object: the request must stay well-formed, and the model can
// recover from a tool_use it cannot act on.
json parse_arguments_object(const json& fn) {
    auto it = fn.find("arguments");
    if (it == fn.end()) return json::object();
    if (it->is_object()) return *it;
    if (!it->is_string()) return json::object();
    json parsed = json::parse(it->get<std::string>(), nullptr, false);
    return (!parsed.is_discarded() && parsed.is_object()) ? parsed
                                                          : json::object();
}

// Assistant tool calls (OpenAI shape) -> Anthropic tool_use blocks.
json to_tool_use_blocks(const json& calls) {
    json blocks = json::array();
    for (const auto& tc : calls) {
        if (!tc.is_object()) continue;
        const json& fn = tc.value("function", json::object());
        std::string name = str_field(fn, "name");
        if (name.empty()) continue;   // placeholder slots are never sent
        json block = {{"type", "tool_use"},
                      {"id", str_field(tc, "id")},
                      {"name", std::move(name)},
                      {"input", parse_arguments_object(fn)}};
        blocks.push_back(std::move(block));
    }
    return blocks;
}

// Anthropic tool_result block for one internal tool message.
json to_tool_result_block(const Message& m) {
    return json{{"type", "tool_result"},
                {"tool_use_id", m.tool_call_id},
                {"content", m.content}};
}

// Anthropic assistant content blocks -> the internal Message. Text blocks join
// into content; tool_use blocks become the internal tool_calls shape so
// dispatch and history stay protocol-agnostic.
Message message_from_blocks(const json& content) {
    Message out;
    out.role = "assistant";
    if (!content.is_array()) {
        out.content = content.is_string() ? content.get<std::string>() : "";
        return out;
    }
    json calls = json::array();
    for (const auto& block : content) {
        if (!block.is_object()) continue;
        const std::string type = str_field(block, "type");
        if (type == "text") {
            out.content += str_field(block, "text");
        } else if (type == "thinking") {
            out.reasoning += str_field(block, "thinking");
        } else if (type == "tool_use") {
            std::string name = str_field(block, "name");
            if (name.empty()) continue;   // unusable call: never enters history
            json input = block.value("input", json::object());
            if (!input.is_object()) input = json::object();
            calls.push_back({{"id", str_field(block, "id")},
                             {"type", "function"},
                             {"function",
                              {{"name", std::move(name)},
                               {"arguments", input.dump()}}}});
        }
    }
    if (!calls.empty()) out.tool_calls = std::move(calls);
    return out;
}

// Anthropic event-stream decoder: named SSE payloads (message_start,
// content_block_start/delta/stop, message_delta, message_stop). The shared
// framing in StreamDecoder hands us one `data:` payload at a time.
class AnthropicStreamDecoder : public StreamDecoder {
public:
    using StreamDecoder::StreamDecoder;

protected:
    void decode_payload(const std::string& data) override {
        json evt = json::parse(data, nullptr, false);
        if (evt.is_discarded()) return;
        const std::string type = str_field(evt, "type");

        if (type == "message_start") {
            const json& usage =
                evt.value("message", json::object()).value("usage", json::object());
            read_usage(usage, "input_tokens", &prompt_tokens_);
            return;
        }
        if (type == "message_delta") {
            read_usage(evt.value("usage", json::object()), "output_tokens",
                       &completion_tokens_);
            return;
        }
        if (type == "content_block_start") {
            const json& block = evt.value("content_block", json::object());
            if (str_field(block, "type") == "tool_use")
                start_tool_block(evt.value("index", 0), block);
            return;
        }
        if (type == "content_block_delta") handle_delta(evt);
    }

    void decode_end() override {
        // Drop tool slots that never received a name (truncated stream), same
        // contract as the OpenAI decoder: only dispatchable calls survive.
        json dense = json::array();
        for (auto& entry : tool_slots_) {
            const json& fn = entry.second.value("function", json::object());
            if (!str_field(fn, "name").empty())
                dense.push_back(std::move(entry.second));
        }
        if (!dense.empty()) out_.tool_calls = std::move(dense);
    }

private:
    static void read_usage(const json& usage, const char* key, long* dst) {
        auto it = usage.find(key);
        if (it != usage.end() && it->is_number()) *dst = it->get<long>();
    }

    void start_tool_block(int index, const json& block) {
        json call = {{"id", str_field(block, "id")},
                     {"type", "function"},
                     {"function",
                      {{"name", str_field(block, "name")},
                       {"arguments", std::string()}}}};
        tool_slots_[index] = std::move(call);
    }

    void handle_delta(const json& evt) {
        const json& delta = evt.value("delta", json::object());
        const std::string delta_type = str_field(delta, "type");
        if (delta_type == "text_delta") {
            StreamChunk chunk;
            chunk.delta = str_field(delta, "text");
            out_.content += chunk.delta;
            emit(chunk);
        } else if (delta_type == "thinking_delta") {
            StreamChunk chunk;
            chunk.reasoning = str_field(delta, "thinking");
            out_.reasoning += chunk.reasoning;
            emit(chunk);
        } else if (delta_type == "input_json_delta") {
            accumulate_input(evt.value("index", 0),
                             str_field(delta, "partial_json"));
        }
    }

    void accumulate_input(int index, const std::string& fragment) {
        auto it = tool_slots_.find(index);
        if (it == tool_slots_.end()) return;
        json& args = it->second["function"]["arguments"];
        args = args.is_string() ? args.get<std::string>() + fragment : fragment;
        StreamChunk chunk;
        chunk.tool_calls = json::array({it->second});
        emit(chunk);
    }

    std::map<int, json> tool_slots_;
};

class AnthropicDialect : public Dialect {
public:
    std::string flavor() const override { return "anthropic"; }

    std::string chat_url(const Config& cfg) const override {
        return cfg.api_base + "/v1/messages";
    }

    std::string models_url(const Config& cfg) const override {
        return cfg.api_base + "/v1/models";
    }

    std::vector<std::string> auth_headers(const Config& cfg) const override {
        std::vector<std::string> headers;
        if (!cfg.api_key.empty())
            headers.push_back("x-api-key: " + cfg.api_key);
        headers.push_back(std::string("anthropic-version: ") + kApiVersion);
        return headers;
    }

    json build_chat_body(const Config& cfg, const std::vector<Message>& messages,
                         const std::vector<std::shared_ptr<Tool>>& tools,
                         bool stream) const override {
        json body = {{"model", cfg.model},
                     {"max_tokens", cfg.max_tokens},   // required by the API
                     {"stream", stream},
                     {"messages", json::array()}};

        // The protocol takes ONE top-level system string: merge every system
        // message (prompt, environment card, memory/skill/brief blocks) in
        // order, exactly like the OpenAI dialect merges them into one block.
        std::string system;
        for (const auto& m : messages) {
            if (m.role != "system") continue;
            if (!system.empty()) system += "\n\n";
            system += m.content;
        }
        if (!system.empty()) body["system"] = system;

        append_messages(body["messages"], messages);
        append_tools(body, tools);
        return body;
    }

    Message parse_completion(const std::string& raw) const override {
        json resp = json::parse(raw, nullptr, false);
        if (resp.is_discarded() || !resp.contains("content")) {
            Message out;
            out.role = "assistant";
            out.content = "[error: malformed LLM response, raw body follows]\n" + raw;
            return out;
        }
        Message out = message_from_blocks(resp["content"]);
        out.content = strip_think(out.content);
        return out;
    }

    std::unique_ptr<StreamDecoder> make_decoder(
        Message& out, StreamDecoder::ChunkSink on_chunk,
        std::string debug_path) const override {
        return std::make_unique<AnthropicStreamDecoder>(out, std::move(on_chunk),
                                                        std::move(debug_path));
    }

    ServerInfo parse_models_response(
        const std::string& body,
        const std::string& preferred_model) const override {
        ServerInfo info;
        for (const ModelInfo& m : parse_model_list_response(body)) {
            if (!preferred_model.empty() && m.id != preferred_model) continue;
            info.model = m.id;
            info.context_size = m.context;   // the API does not report it
            info.context_train = m.context_train;
            info.ok = true;
            break;
        }
        return info;
    }

    std::vector<ModelInfo> parse_model_list_response(
        const std::string& body) const override {
        std::vector<ModelInfo> out;
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("data") || !j["data"].is_array())
            return out;
        for (const auto& e : j["data"]) {
            if (!e.is_object()) continue;
            ModelInfo m;
            m.id = str_field(e, "id");
            if (!m.id.empty()) out.push_back(std::move(m));
        }
        return out;
    }

    TokenUsage parse_usage(const std::string& raw) const override {
        TokenUsage usage;
        json resp = json::parse(raw, nullptr, false);
        const json u = resp.value("usage", json::object());
        if (u.contains("input_tokens") && u["input_tokens"].is_number())
            usage.prompt = u["input_tokens"].get<long>();
        if (u.contains("output_tokens") && u["output_tokens"].is_number())
            usage.completion = u["output_tokens"].get<long>();
        return usage;
    }

    bool is_retryable(long http_code, const std::string&) const override {
        // 429 rate-limit and 5xx (including Anthropic's 529 overloaded) are
        // transient; 4xx otherwise is a genuine rejection.
        return http_code == 429 || http_code >= 500;
    }

    int context_overflow_hint(const std::string& error_body) const override {
        // "prompt is too long: 213456 tokens > 200000 maximum"
        const char* marker = "tokens >";
        auto pos = error_body.find(marker);
        if (pos == std::string::npos) return 0;
        // The maximum is the number AFTER the marker.
        pos += std::char_traits<char>::length(marker);
        while (pos < error_body.size() &&
               !std::isdigit(static_cast<unsigned char>(error_body[pos])))
            ++pos;
        if (pos >= error_body.size()) return 0;
        const long val = std::atol(error_body.c_str() + pos);
        return val > 0 ? static_cast<int>(val) : 0;
    }

private:
    void append_messages(json& out, const std::vector<Message>& messages) const {
        for (const auto& m : messages) {
            if (m.role == "system") continue;   // merged into `system`
            if (m.role == "tool") {
                out.push_back({{"role", kRoleUser},
                               {"content", json::array({to_tool_result_block(m)})}});
                continue;
            }
            if (m.role == "assistant" && !m.tool_calls.is_null() &&
                m.tool_calls.is_array() && !m.tool_calls.empty()) {
                json blocks = json::array();
                if (!m.content.empty())
                    blocks.push_back({{"type", "text"}, {"text", m.content}});
                for (auto& b : to_tool_use_blocks(m.tool_calls))
                    blocks.push_back(std::move(b));
                out.push_back({{"role", kRoleAssistant}, {"content", blocks}});
                continue;
            }
            const char* role =
                (m.role == "assistant") ? kRoleAssistant : kRoleUser;
            // Every other turn is a plain text block; content is always
            // emitted so an empty reply never produces an invalid message.
            json block = {{"type", "text"}, {"text", m.content}};
            json blocks = json::array();
            blocks.push_back(std::move(block));
            out.push_back({{"role", role}, {"content", std::move(blocks)}});
        }
    }

    void append_tools(json& body,
                      const std::vector<std::shared_ptr<Tool>>& tools) const {
        if (tools.empty()) return;
        json arr = json::array();
        for (const auto& t : tools) {
            json schema = t->parameters_schema();
            arr.push_back({{"name", t->name()},
                           {"description", t->description()},
                           {"input_schema", schema}});
        }
        body["tools"] = arr;
    }
};

} // namespace

std::unique_ptr<Dialect> make_anthropic_dialect() {
    return std::make_unique<AnthropicDialect>();
}

} // namespace agent
