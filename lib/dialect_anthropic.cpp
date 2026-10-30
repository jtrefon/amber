
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

// Anthropic's legacy thinking control is a numeric budget. Map the configured
// effort to a documented budget; an unknown effort sends no thinking block.
int reasoning_budget(const std::string& effort) {
    if (effort == "minimal" || effort == "low")
        return 1024;
    if (effort == "medium")
        return 2048;
    if (effort == "high" || effort == "xhigh")
        return 4096;
    if (effort == "max")
        return 8192;
    return 0;
}

bool listed_type_supported(const json& thinking, const char* type) {
    if (!thinking.is_object())
        return false;
    const json& types = thinking.value("types", json::object());
    if (!types.is_object() || !types.contains(type) || !types[type].is_object())
        return false;
    return types[type].value("supported", false);
}

// Which effort to send: the configured one when the model lists it, the
// model's documented default when it does not, and nothing when neither is
// known. Sending an unsupported level is a 400 on every request, so clamping is
// the difference between a switch that works and one that breaks the session.
std::string effective_effort(const Config& cfg) {
    if (cfg.reasoning_effort.empty() || cfg.reasoning_effort == "off" ||
        cfg.reasoning_effort == "minimal")
        return {};
    if (cfg.model_efforts.empty())
        return {};
    if (std::find(cfg.model_efforts.begin(), cfg.model_efforts.end(), cfg.reasoning_effort) !=
        cfg.model_efforts.end())
        return cfg.reasoning_effort;
    return cfg.model_default_effort;
}

// The thinking block, in the shape the model advertises. Adaptive models take
// output_config.effort; legacy models take a token budget. "off" is sent only
// when the capability says the type is accepted: an always-on model is left
// with its default rather than sent a block that 400s.
bool thinking_off(const Config& cfg) {
    return cfg.reasoning_effort.empty() || cfg.reasoning_effort == "off" ||
           cfg.reasoning_effort == "minimal";
}

// "Off" is only sent as a type the model advertises. An always-on model is
// left with its default rather than sent a block the API would reject.
void append_off_thinking(json& body, const Config& cfg) {
    if (cfg.model_thinking_disabled_ok)
        body["thinking"] = {{"type", "disabled"}};
    else if (cfg.model_thinking_between_tools_ok)
        body["thinking"] = {{"type", "between_tools"}};
}

void append_adaptive_thinking(json& body, const Config& cfg) {
    if (thinking_off(cfg)) {
        append_off_thinking(body, cfg);
        return;
    }
    body["thinking"] = {{"type", "adaptive"}};
    if (cfg.show_reasoning)
        body["thinking"]["display"] = "summarized";
    const std::string effort = effective_effort(cfg);
    if (!effort.empty())
        body["output_config"] = {{"effort", effort}};
}

void append_legacy_thinking(json& body, const Config& cfg, size_t max_tokens) {
    if (thinking_off(cfg)) {
        if (cfg.model_thinking_disabled_ok)
            body["thinking"] = {{"type", "disabled"}};
        return;
    }
    // A legacy model without an effort list (Sonnet/Haiku 4.5) still takes a
    // budget; one with a list uses the clamped effort, so an unsupported level
    // neither over-budgets nor sends an effort the model rejects.
    const std::string effort = effective_effort(cfg);
    const std::string budget_effort = effort.empty() ? cfg.reasoning_effort : effort;
    const int chosen =
        cfg.thinking_budget > 0 ? cfg.thinking_budget : reasoning_budget(budget_effort);
    const int budget = std::min(chosen, static_cast<int>(max_tokens) - 1);
    if (budget >= 1024)
        body["thinking"] = {{"type", "enabled"}, {"budget_tokens", budget}};
    if (!effort.empty())
        body["output_config"] = {{"effort", effort}};
}

void append_thinking(json& body, const Config& cfg, size_t max_tokens) {
    if (!cfg.model_supports_reasoning)
        return;
    if (cfg.model_thinking_adaptive)
        append_adaptive_thinking(body, cfg);
    else if (cfg.model_thinking_legacy)
        append_legacy_thinking(body, cfg, max_tokens);
}

// One listing entry: reported limits win; a known family fills the gaps; an
// unknown id stays 0. The Models API publishes output limits as `max_tokens`;
// `max_output_tokens` is accepted as the older/gateway spelling.
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
    m.max_output = read_limit("max_tokens");
    if (m.max_output == 0)
        m.max_output = read_limit("max_output_tokens");
    m.line = str_field(e, "line");
    apply_claude_family(m);

    // The capabilities block is authoritative where it speaks. Absent fields
    // keep the family answer; a field that says "not supported" must win.
    const json caps = e.value("capabilities", json::object());
    const json thinking =
        caps.is_object() ? caps.value("thinking", json::object()) : json::object();
    if (thinking.is_object() && thinking.contains("types")) {
        if (thinking["types"].contains("disabled"))
            m.thinking_disabled_ok = listed_type_supported(thinking, "disabled");
        if (thinking["types"].contains("between_tools"))
            m.thinking_between_tools_ok = listed_type_supported(thinking, "between_tools");
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

// A prompt cache is a prefix cache, and the Messages API only caches what is
// explicitly marked: without a breakpoint every request is a full-price
// prefill even when the prefix is byte-identical. "off" is the user's escape
// hatch; "1h" is the extended lifetime (higher write price, survives a slow
// human-paced turn).
bool prompt_caching(const Config& cfg) {
    return cfg.prompt_cache_ttl != "off";
}

void mark_cacheable(json& block, const Config& cfg) {
    block["cache_control"] = {{"type", "ephemeral"}};
    if (cfg.prompt_cache_ttl == "1h")
        block["cache_control"]["ttl"] = "1h";
}

// The moving breakpoint: everything up to and including the newest user turn
// (a prompt, or the tool results answering the last call) is reused by the next
// request. Only own-built user blocks are eligible -- a replayed assistant turn
// can carry signed thinking blocks, which must stay byte-identical.
void mark_last_turn_cacheable(json& messages, const Config& cfg) {
    if (!prompt_caching(cfg) || !messages.is_array() || messages.empty())
        return;
    json& last = messages.back();
    if (last.value("role", std::string()) != kRoleUser)
        return;
    if (!last.contains("content") || !last["content"].is_array() || last["content"].empty())
        return;
    json& block = last["content"].back();
    if (!block.is_object())
        return;
    const std::string type = str_field(block, "type");
    if (type != "text" && type != "tool_result")
        return;
    mark_cacheable(block, cfg);
}

// The top-level system field when caching is on: one block per system message,
// the FIRST (the stable prompt) carrying the breakpoint and the injected blocks
// after it. A change in an injected block then leaves the cached prefix alone.
json cached_system_blocks(const std::vector<Message>& messages, const Config& cfg) {
    json blocks = json::array();
    bool marked = false;
    for (const auto& m : messages) {
        if (m.role != "system" || m.content.empty())
            continue;
        json block = text_block(m.content);
        if (!marked) {
            mark_cacheable(block, cfg);
            marked = true;
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

// The top-level system field when caching is off: the protocol's plain string
// form, every system message joined in order.
std::string merged_system(const std::vector<Message>& messages) {
    std::string system;
    for (const auto& m : messages) {
        if (m.role != "system")
            continue;
        if (!system.empty())
            system += "\n\n";
        system += m.content;
    }
    return system;
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
    // Keep the protocol's own blocks: thinking blocks carry a signature the
    // API requires back verbatim on the next turn, and a tool-use turn cannot
    // be reconstructed from text plus parsed arguments without losing it.
    out.raw_content = content;
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
            read_usage(usage, "cache_read_input_tokens", &cache_read_tokens_);
            read_usage(usage, "cache_creation_input_tokens", &cache_write_tokens_);
            return;
        }
        if (type == "message_delta") {
            read_usage(evt.value("usage", json::object()), "output_tokens", &completion_tokens_);
            return;
        }
        if (type == "content_block_start") {
            const int index = evt.value("index", 0);
            const json& block = evt.value("content_block", json::object());
            start_block(index, block);
            if (str_field(block, "type") == "tool_use")
                start_tool_block(index, block);
            return;
        }
        if (type == "content_block_delta")
            handle_delta(evt);
    }

    void decode_end() override {
        // Raw blocks first: they read the tool slots, which the dispatch shape
        // below moves out.
        finish_blocks();
        // Drop tool slots that never received a name (truncated stream), same
        // contract as the OpenAI decoder: only dispatchable calls survive.
        json dense = json::array();
        for (auto& entry : tool_slots_) {
            const json& fn = entry.second.value("function", json::object());
            if (!str_field(fn, "name").empty())
                dense.push_back(entry.second);
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

    void start_block(int index, const json& block) {
        const std::string type = str_field(block, "type");
        if (type == "thinking")
            blocks_[index] = {{"type", "thinking"}, {"thinking", ""}, {"signature", ""}};
        else if (type == "text")
            blocks_[index] = {{"type", "text"}, {"text", ""}};
        else if (type == "tool_use")
            blocks_[index] = {{"type", "tool_use"},
                              {"id", str_field(block, "id")},
                              {"name", str_field(block, "name")},
                              {"input", json::object()}};
    }

    void finish_blocks() {
        json raw = json::array();
        for (auto& [index, block] : blocks_) {
            (void)index;
            const std::string type = str_field(block, "type");
            if (type == "text" && str_field(block, "text").empty())
                continue;
            if (type == "thinking" && str_field(block, "thinking").empty() &&
                str_field(block, "signature").empty())
                continue;
            if (type == "tool_use") {
                const std::string name = str_field(block, "name");
                if (name.empty())
                    continue;
                auto slot = tool_slots_.find(index);
                if (slot != tool_slots_.end()) {
                    block["id"] = str_field(slot->second, "id");
                    block["input"] =
                        parse_arguments_object(slot->second.value("function", json::object()));
                }
            }
            raw.push_back(std::move(block));
        }
        if (!raw.empty())
            out_.raw_content = std::move(raw);
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
        const int index = evt.value("index", 0);
        if (delta_type == "text_delta") {
            const std::string text = str_field(delta, "text");
            auto it = blocks_.find(index);
            if (it != blocks_.end() && str_field(it->second, "type") == "text")
                it->second["text"] = str_field(it->second, "text") + text;
            StreamChunk chunk;
            chunk.delta = text;
            out_.content += chunk.delta;
            emit(chunk);
        } else if (delta_type == "thinking_delta") {
            const std::string text = str_field(delta, "thinking");
            auto it = blocks_.find(index);
            if (it != blocks_.end() && str_field(it->second, "type") == "thinking")
                it->second["thinking"] = str_field(it->second, "thinking") + text;
            StreamChunk chunk;
            chunk.reasoning = text;
            out_.reasoning += chunk.reasoning;
            emit(chunk);
        } else if (delta_type == "signature_delta") {
            auto it = blocks_.find(index);
            if (it != blocks_.end() && str_field(it->second, "type") == "thinking")
                it->second["signature"] =
                    str_field(it->second, "signature") + str_field(delta, "signature");
        } else if (delta_type == "input_json_delta") {
            accumulate_input(index, str_field(delta, "partial_json"));
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
    std::map<int, json> blocks_; // every content block, in stream order
};

void append_tools_json(json& body, const std::vector<std::shared_ptr<Tool>>& tools) {
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

// ONE user message carries every tool_result of the turn, the answers for
// ids whose result never arrived, and a user turn landing right after the
// results (as text, so the roles still alternate).
void append_tool_results(json& out, const std::vector<Message>& messages, std::size_t& i,
                         std::vector<std::string>& pending) {
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
                                std::size_t& next_id) {
    flush_pending(out, pending);
    // A turn that came from this protocol is replayed verbatim: thinking
    // blocks carry a signature only the provider can produce, and the API
    // rejects a tool-use turn whose thinking blocks were dropped/modified.
    if (m.raw_content.is_array() && !m.raw_content.empty()) {
        for (const auto& b : m.raw_content) {
            if (!b.is_object() || str_field(b, "type") != "tool_use")
                continue;
            const std::string id = str_field(b, "id");
            if (!id.empty())
                pending.push_back(id);
        }
        out.push_back({{"role", kRoleAssistant}, {"content", m.raw_content}});
        return;
    }
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
void append_text_turn(json& out, const Message& m, std::vector<std::string>& pending) {
    if (m.content.empty())
        return;
    if (m.role == "assistant") {
        flush_pending(out, pending);
        const json content = (m.raw_content.is_array() && !m.raw_content.empty())
                                 ? m.raw_content
                                 : json::array({text_block(m.content)});
        out.push_back({{"role", kRoleAssistant}, {"content", content}});
        return;
    }
    json blocks = take_answers(pending);
    blocks.push_back(text_block(m.content));
    out.push_back({{"role", kRoleUser}, {"content", std::move(blocks)}});
}

// The internal history keeps one message per tool result, and a turn may
// hold results that never arrived; the API's contract is ONE message
// answering every tool_use of the turn that just ended, immediately after
// it. The appenders below reconcile the two, one message shape each. A
// history restored from a session file can predate any of this, so the
// walk repairs rather than trusts: that is the difference between a valid
// replay and a 400 on every later request in the session.
void append_messages(json& out, const std::vector<Message>& messages) {
    std::vector<std::string> pending; // tool_use ids awaiting a tool_result
    std::size_t next_id = 0;          // numbering for calls that arrived without one
    std::size_t i = 0;
    while (i < messages.size()) {
        const Message& m = messages[i];
        if (m.role == "system") {
            ++i; // merged into `system`
        } else if (m.role == "tool") {
            append_tool_results(out, messages, i, pending);
        } else if (m.role == "assistant" && !m.tool_calls.is_null() && m.tool_calls.is_array() &&
                   !m.tool_calls.empty()) {
            append_assistant_tool_turn(out, m, pending, next_id);
            ++i;
        } else {
            append_text_turn(out, m, pending);
            ++i;
        }
    }
    flush_pending(out, pending);
}

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

        // The protocol takes ONE top-level system field: it is an array of
        // blocks when prompt caching is on, so the breakpoint can sit between
        // the stable prompt and the injected blocks that change between turns.
        // (A change on the wrong side of the breakpoint re-prefills the whole
        // conversation behind it.)
        if (prompt_caching(cfg)) {
            json blocks = cached_system_blocks(messages, cfg);
            if (!blocks.empty())
                body["system"] = std::move(blocks);
        } else if (const std::string system = merged_system(messages); !system.empty()) {
            body["system"] = system;
        }

        // The model's documented reasoning contract: adaptive + effort on
        // 5.x/4.6+, a budget on 4.5 and older, and no block where the listing
        // said nothing. See append_thinking.
        append_thinking(body, cfg, max_tokens);

        append_messages(body["messages"], messages);
        mark_last_turn_cacheable(body["messages"], cfg);
        if (tools_advertised(cfg))
            append_tools_json(body, tools);
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
            info.line = m.line;
            info.thinking_adaptive = m.thinking_adaptive;
            info.thinking_legacy = m.thinking_legacy;
            info.thinking_always_on = m.thinking_always_on;
            info.thinking_disabled_ok = m.thinking_disabled_ok;
            info.thinking_between_tools_ok = m.thinking_between_tools_ok;
            info.efforts = m.efforts;
            info.default_effort = m.default_effort;
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
        if (u.contains("cache_read_input_tokens") && u["cache_read_input_tokens"].is_number())
            usage.cache_read = u["cache_read_input_tokens"].get<long>();
        if (u.contains("cache_creation_input_tokens") &&
            u["cache_creation_input_tokens"].is_number())
            usage.cache_write = u["cache_creation_input_tokens"].get<long>();
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
};
} // namespace

std::unique_ptr<Dialect> make_anthropic_dialect() {
    return std::make_unique<AnthropicDialect>();
}

} // namespace agent
