
#include "agent/dialect_anthropic.h"

#include "agent/agent_helpers.h"

#include <algorithm>
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

// A tool_use whose tool_result never arrived (a cancelled or failed dispatch)
// still needs an answer: the API requires one for every id in the message
// immediately after the assistant blocks, and an unanswered id rejects every
// later request in the session. The empty result needs an answer too -- an
// empty string reads as a missing block.
constexpr const char* kNoResultRecorded = "(no result recorded)";
constexpr const char* kNoOutput = "(no output)";

// Defensive string read: a non-string/absent field yields "", never a
// type_error — response bodies are untrusted input and an exception thrown
// inside a curl write callback cannot be recovered from.
std::string str_field(const json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

// Documented family defaults for listings that omit the limits (a corporate
// proxy may return ids only). These are constants, not a guess: an id outside
// the table stays 0. The 1M context beta is opt-in and remains a user pin.
struct ClaudeFamily {
    const char* prefix;
    int context;
    int max_output;
    bool reasoning;
};

constexpr ClaudeFamily kClaudeFamilies[] = {
    {"claude-3-5-", 200000, 8192, false},     {"claude-3-7-", 200000, 64000, true},
    {"claude-sonnet-4", 200000, 64000, true}, {"claude-opus-4", 200000, 32000, true},
    {"claude-haiku-4", 200000, 64000, true},  {"claude-3-opus", 200000, 4096, false},
    {"claude-3-sonnet", 200000, 4096, false}, {"claude-3-haiku", 200000, 4096, false},
    {"claude-2.1", 200000, 4096, false},      {"claude-2", 100000, 4096, false},
    {"claude-instant", 100000, 4096, false},
};

const ClaudeFamily* claude_family(const std::string& id) {
    for (const auto& family : kClaudeFamilies)
        if (id.rfind(family.prefix, 0) == 0)
            return &family;
    return nullptr;
}

// Anthropic's reasoning control is a numeric thinking budget, not an effort
// tag. Map the configured effort to a documented budget; an unknown effort
// sends no thinking block.
int reasoning_budget(const std::string& effort) {
    if (effort == "minimal" || effort == "low")
        return 1024;
    if (effort == "medium")
        return 2048;
    if (effort == "high" || effort == "xhigh")
        return 4096;
    return 0;
}

// One listing entry: reported limits win; a known family fills the gaps; an
// unknown id stays 0. Free rather than a class member so the dialect type does
// not grow past its size cap.
ModelInfo parse_model_entry(const json& e) {
    ModelInfo m;
    m.id = str_field(e, "id");
    const auto read_limit = [&](const char* key) -> int {
        auto it = e.find(key);
        return it != e.end() && it->is_number() && it->get<long>() > 0
                   ? static_cast<int>(it->get<long>())
                   : 0;
    };
    m.context = read_limit("max_input_tokens");
    m.max_output = read_limit("max_output_tokens");
    if (const ClaudeFamily* family = claude_family(m.id)) {
        if (m.context == 0)
            m.context = family->context;
        if (m.max_output == 0)
            m.max_output = family->max_output;
        m.reasoning = family->reasoning;
    }
    return m;
}

// Parse a tool call's `arguments` (a JSON string on the internal model) into
// the object Anthropic expects for tool_use.input. Malformed arguments yield
// an empty object: the request must stay well-formed, and the model can
// recover from a tool_use it cannot act on.
json parse_arguments_object(const json& fn) {
    auto it = fn.find("arguments");
    if (it == fn.end())
        return json::object();
    if (it->is_object())
        return *it;
    if (!it->is_string())
        return json::object();
    json parsed = json::parse(it->get<std::string>(), nullptr, false);
    return (!parsed.is_discarded() && parsed.is_object()) ? parsed : json::object();
}

// Assistant tool calls (OpenAI shape) -> Anthropic tool_use blocks. A call
// that reached history without an id (a text-extracted <tool_call>, a router
// that omits ids) still needs one: every tool_use carries a non-empty id so
// its tool_result has something to pair with. The numbering derives from the
// history alone, so replaying the same conversation sends the same ids.
json to_tool_use_blocks(const json& calls, std::size_t& next_id) {
    json blocks = json::array();
    for (const auto& tc : calls) {
        if (!tc.is_object())
            continue;
        const json& fn = tc.value("function", json::object());
        std::string name = str_field(fn, "name");
        if (name.empty())
            continue; // placeholder slots are never sent
        std::string id = str_field(tc, "id");
        if (id.empty())
            id = "amber_call_" + std::to_string(next_id++);
        json block = {{"type", "tool_use"},
                      {"id", std::move(id)},
                      {"name", std::move(name)},
                      {"input", parse_arguments_object(fn)}};
        blocks.push_back(std::move(block));
    }
    return blocks;
}

// Anthropic tool_result block answering `id`. An empty result reads as a
// missing block to the API; say so instead.
json tool_result_block(const std::string& id, const std::string& content) {
    return json{{"type", "tool_result"},
                {"tool_use_id", id},
                {"content", content.empty() ? kNoOutput : content}};
}

// The answers for tool_use ids whose tool_result never arrived: the API wants
// one for every id in the message immediately after the assistant blocks, and
// an unanswered id rejects every later request in the session.
json take_answers(std::vector<std::string>& pending) {
    json blocks = json::array();
    for (const auto& id : pending)
        blocks.push_back(
            {{"type", "tool_result"}, {"tool_use_id", id}, {"content", kNoResultRecorded}});
    pending.clear();
    return blocks;
}

// ONE user message answers every id still waiting. Its own message, because
// the answers close the turn that just ended: a later assistant message comes
// after them, never carries them.
void flush_pending(json& out, std::vector<std::string>& pending) {
    json answers = take_answers(pending);
    if (!answers.empty())
        out.push_back({{"role", kRoleUser}, {"content", std::move(answers)}});
}

// The id a tool message answers: its own when the turn asked for it, the
// oldest unanswered id when it carries none (a call that reached history
// without one), and "" for a result nobody is waiting for -- a stray
// tool_result has no valid place in the protocol.
std::string result_id_for(const Message& m, std::vector<std::string>& pending) {
    if (!m.tool_call_id.empty()) {
        auto it = std::find(pending.begin(), pending.end(), m.tool_call_id);
        if (it == pending.end())
            return {};
        pending.erase(it);
        return m.tool_call_id;
    }
    if (pending.empty())
        return {};
    const std::string id = pending.front();
    pending.erase(pending.begin());
    return id;
}

json text_block(const std::string& text) {
    return json{{"type", "text"}, {"text", text}};
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
        if (!block.is_object())
            continue;
        const std::string type = str_field(block, "type");
        if (type == "text") {
            out.content += str_field(block, "text");
        } else if (type == "thinking") {
            out.reasoning += str_field(block, "thinking");
        } else if (type == "tool_use") {
            std::string name = str_field(block, "name");
            if (name.empty())
                continue; // unusable call: never enters history
            json input = block.value("input", json::object());
            if (!input.is_object())
                input = json::object();
            calls.push_back(
                {{"id", str_field(block, "id")},
                 {"type", "function"},
                 {"function", {{"name", std::move(name)}, {"arguments", input.dump()}}}});
        }
    }
    if (!calls.empty())
        out.tool_calls = std::move(calls);
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
        if (evt.is_discarded())
            return;
        const std::string type = str_field(evt, "type");

        if (type == "message_start") {
            const json& usage = evt.value("message", json::object()).value("usage", json::object());
            read_usage(usage, "input_tokens", &prompt_tokens_);
            return;
        }
        if (type == "message_delta") {
            read_usage(evt.value("usage", json::object()), "output_tokens", &completion_tokens_);
            return;
        }
        if (type == "content_block_start") {
            const json& block = evt.value("content_block", json::object());
            if (str_field(block, "type") == "tool_use")
                start_tool_block(evt.value("index", 0), block);
            return;
        }
        if (type == "content_block_delta")
            handle_delta(evt);
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
        if (!dense.empty())
            out_.tool_calls = std::move(dense);
    }

private:
    static void read_usage(const json& usage, const char* key, long* dst) {
        auto it = usage.find(key);
        if (it != usage.end() && it->is_number())
            *dst = it->get<long>();
    }

    void start_tool_block(int index, const json& block) {
        json call = {
            {"id", str_field(block, "id")},
            {"type", "function"},
            {"function", {{"name", str_field(block, "name")}, {"arguments", std::string()}}}};
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
            accumulate_input(evt.value("index", 0), str_field(delta, "partial_json"));
        }
    }

    void accumulate_input(int index, const std::string& fragment) {
        auto it = tool_slots_.find(index);
        if (it == tool_slots_.end())
            return;
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
        return versioned_url(cfg.api_base, "/v1", "/messages");
    }

    std::string models_url(const Config& cfg) const override {
        // The listing paginates (default 20, max 1000). Ask for one page: a
        // provider file whose models silently stopped at 20 would read as a
        // truncated catalogue, not as pagination.
        return versioned_url(cfg.api_base, "/v1", "/models?limit=1000");
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
        const size_t max_tokens = request_max_tokens(cfg);
        json body = {{"model", cfg.model},
                     {"max_tokens", max_tokens}, // required by the API, capped to the model
                     {"stream", stream},
                     {"messages", json::array()}};

        // The protocol takes ONE top-level system string: merge every system
        // message (prompt, environment card, memory/skill/brief blocks) in
        // order, exactly like the OpenAI dialect merges them into one block.
        std::string system;
        for (const auto& m : messages) {
            if (m.role != "system")
                continue;
            if (!system.empty())
                system += "\n\n";
            system += m.content;
        }
        if (!system.empty())
            body["system"] = system;

        // Anthropic extended thinking is a numeric budget, not a tag. Only a
        // model that advertises reasoning gets it, and the budget must stay
        // below max_tokens (the API's own rule).
        if (cfg.model_supports_reasoning && !cfg.reasoning_effort.empty() &&
            cfg.reasoning_effort != "off" && max_tokens > 1024) {
            int budget = cfg.thinking_budget > 0 ? cfg.thinking_budget
                                                 : reasoning_budget(cfg.reasoning_effort);
            budget = std::min(budget, static_cast<int>(max_tokens) - 1);
            if (budget >= 1024)
                body["thinking"] = {{"type", "enabled"}, {"budget_tokens", budget}};
        }

        append_messages(body["messages"], messages);
        if (tools_advertised(cfg))
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

    std::unique_ptr<StreamDecoder> make_decoder(Message& out, StreamDecoder::ChunkSink on_chunk,
                                                std::string debug_path) const override {
        return std::make_unique<AnthropicStreamDecoder>(out, std::move(on_chunk),
                                                        std::move(debug_path));
    }

    ServerInfo parse_models_response(const std::string& body,
                                     const std::string& preferred_model) const override {
        ServerInfo info;
        for (const ModelInfo& m : parse_model_list_response(body)) {
            if (!preferred_model.empty() && m.id != preferred_model)
                continue;
            info.model = m.id;
            info.context_size = m.context; // max_input_tokens, 0 when unreported
            info.context_train = m.context_train;
            info.max_output = m.max_output;
            info.reasoning = m.reasoning;
            info.supported_parameters = m.supported_parameters;
            info.ok = true;
            break;
        }
        return info;
    }

    std::vector<ModelInfo> parse_model_list_response(const std::string& body) const override {
        std::vector<ModelInfo> out;
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("data") || !j["data"].is_array())
            return out;
        for (const auto& e : j["data"]) {
            if (!e.is_object())
                continue;
            ModelInfo m = parse_model_entry(e);
            if (!m.id.empty())
                out.push_back(std::move(m));
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
        if (pos == std::string::npos)
            return 0;
        // The maximum is the number AFTER the marker.
        pos += std::char_traits<char>::length(marker);
        while (pos < error_body.size() &&
               !std::isdigit(static_cast<unsigned char>(error_body[pos])))
            ++pos;
        if (pos >= error_body.size())
            return 0;
        const long val = std::atol(error_body.c_str() + pos);
        return val > 0 ? static_cast<int>(val) : 0;
    }

private:
    // The internal history keeps one message per tool result, and a turn may
    // hold results that never arrived; the API's contract is ONE message
    // answering every tool_use of the turn that just ended, immediately after
    // it. The appenders below reconcile the two, one message shape each. A
    // history restored from a session file can predate any of this, so the
    // walk repairs rather than trusts: that is the difference between a valid
    // replay and a 400 on every later request in the session.
    void append_messages(json& out, const std::vector<Message>& messages) const {
        std::vector<std::string> pending; // tool_use ids awaiting a tool_result
        std::size_t next_id = 0;          // numbering for calls that arrived without one
        std::size_t i = 0;
        while (i < messages.size()) {
            const Message& m = messages[i];
            if (m.role == "system") {
                ++i; // merged into `system`
            } else if (m.role == "tool") {
                append_tool_results(out, messages, i, pending);
            } else if (m.role == "assistant" && !m.tool_calls.is_null() &&
                       m.tool_calls.is_array() && !m.tool_calls.empty()) {
                append_assistant_tool_turn(out, m, pending, next_id);
                ++i;
            } else {
                append_text_turn(out, m, pending);
                ++i;
            }
        }
        flush_pending(out, pending);
    }

    // ONE user message carries every tool_result of the turn, the answers for
    // ids whose result never arrived, and a user turn landing right after the
    // results (as text, so the roles still alternate).
    void append_tool_results(json& out, const std::vector<Message>& messages, std::size_t& i,
                             std::vector<std::string>& pending) const {
        json blocks = json::array();
        for (; i < messages.size() && messages[i].role == "tool"; ++i) {
            const std::string id = result_id_for(messages[i], pending);
            if (!id.empty())
                blocks.push_back(tool_result_block(id, messages[i].content));
        }
        for (auto& b : take_answers(pending))
            blocks.push_back(std::move(b));
        if (i < messages.size() && messages[i].role == kRoleUser && !messages[i].content.empty()) {
            blocks.push_back(text_block(messages[i].content));
            ++i;
        }
        if (!blocks.empty())
            out.push_back({{"role", kRoleUser}, {"content", std::move(blocks)}});
    }

    // An assistant turn that asked for tools: the previous turn's unanswered
    // ids close in a user message of their own first, then text (when any)
    // and tool_use blocks. The emitted ids become the set the next message
    // must answer.
    void append_assistant_tool_turn(json& out, const Message& m, std::vector<std::string>& pending,
                                    std::size_t& next_id) const {
        flush_pending(out, pending);
        json blocks = json::array();
        if (!m.content.empty())
            blocks.push_back(text_block(m.content));
        for (auto& b : to_tool_use_blocks(m.tool_calls, next_id)) {
            const std::string id = str_field(b, "id");
            if (!id.empty())
                pending.push_back(id);
            blocks.push_back(std::move(b));
        }
        if (!blocks.empty())
            out.push_back({{"role", kRoleAssistant}, {"content", std::move(blocks)}});
    }

    // A plain text turn. Anthropic rejects an empty text block -- "text content
    // block must be non-empty" -- where OpenAI tolerates "". A turn with no
    // text (a cancelled run, a reply stripped to nothing, an empty message
    // already in a saved session) is skipped rather than sent as an empty
    // block: one 400s the whole request, and then every later request in that
    // session.
    void append_text_turn(json& out, const Message& m, std::vector<std::string>& pending) const {
        if (m.content.empty())
            return;
        if (m.role == "assistant") {
            flush_pending(out, pending);
            out.push_back(
                {{"role", kRoleAssistant}, {"content", json::array({text_block(m.content)})}});
            return;
        }
        json blocks = take_answers(pending);
        blocks.push_back(text_block(m.content));
        out.push_back({{"role", kRoleUser}, {"content", std::move(blocks)}});
    }

    void append_tools(json& body, const std::vector<std::shared_ptr<Tool>>& tools) const {
        if (tools.empty())
            return;
        json arr = json::array();
        for (const auto& t : tools) {
            json schema = t->parameters_schema();
            arr.push_back(
                {{"name", t->name()}, {"description", t->description()}, {"input_schema", schema}});
        }
        body["tools"] = arr;
    }
};

} // namespace

std::unique_ptr<Dialect> make_anthropic_dialect() {
    return std::make_unique<AnthropicDialect>();
}

} // namespace agent
