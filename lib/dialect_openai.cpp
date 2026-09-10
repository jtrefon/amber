
#include "agent/dialect_openai.h"

#include "agent/agent_helpers.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

namespace agent {

namespace {

auto read_int = [](const json& o, const char* k) -> int {
    auto it = o.find(k);
    return (it != o.end() && it->is_number_integer()) ? it->get<int>() : 0;
};

void sanitize_node(json& node) {
    if (!node.is_object()) {
        node = json{{"type", "object"}};
        return;
    }
    auto t = node.find("type");
    if (t == node.end() || !t->is_string())
        node["type"] = "object";
    // The nlohmann {"required", {}} gotcha produces "required": null, which
    // makes llama.cpp's grammar generator throw "type must be array, but is
    // null". Repair any non-array required to an empty array.
    auto req = node.find("required");
    if (req != node.end() && !req->is_array())
        node["required"] = json::array();
    if (node["type"] == "array") {
        auto items = node.find("items");
        if (items == node.end() || !items->is_object())
            node["items"] = json::object();
        else
            sanitize_node(node["items"]);
    }
    auto props = node.find("properties");
    if (props != node.end() && props->is_object())
        for (auto& [_, v] : props->items())
            sanitize_node(v);
}

// Accumulate a tool-call `arguments` fragment onto the running value. Servers
// differ in how they stream arguments: most send a JSON *string* in pieces
// (concatenate), while some send a complete JSON *object* in one delta (assign
// and re-serialize). We always keep `arguments` as a JSON *string* so the
// resulting tool_calls match the OpenAI wire contract; an object fragment is
// merged into an in-memory object view and re-serialized rather than stored as
// a raw JSON object (object-typed arguments sent back to the API on the next
// turn corrupt the conversation and arrive at the tool as `{}`).
void accumulate_arguments(json& fn, const json& frag) {
    auto view = [&]() -> json {
        if (fn.contains("arguments") && fn["arguments"].is_string()) {
            json v = json::parse(fn["arguments"].get<std::string>(), nullptr,
                                  false);
            if (!v.is_discarded() && v.is_object()) return v;
        }
        return json::object();
    };
    if (frag.is_string()) {
        std::string piece = frag.get<std::string>();
        if (!fn.contains("arguments")) {
            fn["arguments"] = piece;
            return;
        }
        if (fn["arguments"].is_string()) {
            std::string cur = fn["arguments"].get<std::string>();
            json cur_obj = json::parse(cur, nullptr, false);
            json piece_obj = json::parse(piece, nullptr, false);
            if (!cur_obj.is_discarded() && cur_obj.is_object() &&
                !piece_obj.is_discarded() && piece_obj.is_object()) {
                for (auto it = piece_obj.begin(); it != piece_obj.end(); ++it)
                    cur_obj[it.key()] = it.value();
                fn["arguments"] = cur_obj.dump();
            } else if (!cur_obj.is_discarded() && cur_obj.is_object()) {
                return;
            } else {
                fn["arguments"] = cur + piece;
            }
            return;
        }
        fn["arguments"] = piece;
        return;
    }
    if (frag.is_object()) {
        json base = view();
        for (auto it = frag.begin(); it != frag.end(); ++it)
            base[it.key()] = it.value();
        fn["arguments"] = base.dump();
        return;
    }
    fn["arguments"] = frag.dump();
}

// A tool-call slot is "real" once it carries a function name. Slots that never
// became real are sparse-index placeholders: some gateways (e.g. kilocode
// routing to MiniMax) stream tool calls with 1-based `index` values, so slot 0
// stays an empty {} unless we drop it. Emitting `{}` as a tool call makes
// dispatch log "unknown tool: " and poisons history replay. An id-only slot
// (name never arrived — truncated stream) is equally unusable: dispatching it
// would deny an "unknown tool" and leave an orphan tool-result referencing a
// call that no longer exists.
void drop_empty_tool_slots(json& calls) {
    if (calls.is_null() || !calls.is_array()) return;
    json dense = json::array();
    for (auto& tc : calls) {
        if (!tc.is_object()) continue;
        const json& fn = tc.value("function", json::object());
        bool has_name = fn.is_object() && fn.contains("name") &&
                        fn["name"].is_string() &&
                        !fn["name"].get<std::string>().empty();
        if (has_name) dense.push_back(std::move(tc));
    }
    calls = std::move(dense);
}

// Normalize an assistant message's tool_calls for the wire: drop entries that
// never received a function name (e.g. "{}" placeholder slots persisted by
// older parsers on sparse-index streams — a strict gateway rejects them with
// a type-discriminator 400) and default a missing/empty `type` to "function".
// The caller's history is untouched.
json sanitize_tool_calls(const json& calls) {
    if (calls.is_null() || !calls.is_array()) return json::array();
    json out = json::array();
    for (const auto& tc : calls) {
        if (!tc.is_object()) continue;
        const json& fn = tc.value("function", json::object());
        if (!fn.is_object()) continue;
        auto it = fn.find("name");
        if (it == fn.end() || !it->is_string() ||
            it->get<std::string>().empty())
            continue;
        json kept = tc;
        // Some paths (text-extracted calls, restored sessions) omit `type`;
        // the OpenAI contract requires the discriminator.
        if (!kept.contains("type") || !kept["type"].is_string() ||
            kept["type"].get<std::string>().empty())
            kept["type"] = "function";
        out.push_back(std::move(kept));
    }
    return out;
}

// Extract a string field defensively: returns d if missing, null, or not a
// string (so a malformed model response never throws and aborts the turn).
std::string str_or_raw(const json& j, const char* key, const std::string& d) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return d;
    if (it->is_string()) return it->get<std::string>();
    // Non-string content: keep it as JSON text rather than throwing, so the
    // pipeline can feed it back to the model instead of crashing.
    return it->dump();
}

// Try to extract context_size from an HTTP 400 error body.
// Returns > 0 if a pattern like "maximum context length is NNNN" is found,
// or 0 if no known pattern matches.
int parse_context_size_from_error(const std::string& body) {
    // Common error patterns across providers:
    // "maximum context length is 8192 tokens"
    // "max context length: 4096"
    // "n_ctx is 2048"
    // "context length exceeds 16384"
    // "model's maximum context length is 128K"
    // "Request exceeds maximum context length (4096 tokens)"
    const char* patterns[] = {
        "maximum context length is ",
        "max context length: ",
        "max context length is ",
        "n_ctx is ",
        "n_ctx = ",
        "context length exceeds ",
        "maximum context length (",
        "context length of ",
    };
    for (const char* pat : patterns) {
        auto pos = body.find(pat);
        if (pos == std::string::npos) continue;
        pos += strlen(pat);
        // Skip past any non-digit prefix (e.g. open paren)
        while (pos < body.size() && !std::isdigit(static_cast<unsigned char>(body[pos])))
            ++pos;
        if (pos >= body.size()) continue;
        long val = std::atol(body.c_str() + pos);
        if (val > 0 && val < 10000000) // sanity: 1M tokens is generous max
            return static_cast<int>(val);
    }
    return 0;
}

// True when a non-2xx response is a transient upstream failure rather than a
// request rejection. Gateways (kilocode's OpenAI-compatible router among
// them) surface an overloaded/crashed upstream as HTTP 400 whose body is an
// empty SSE stream (at most comments / a bare [DONE]) — retrying that shape
// rides through the blip, while a genuine schema-rejection 400 (JSON error
// body) stays non-retryable.
bool is_retryable_http_error(long http_code, const std::string& body) {
    if (http_code == 429 || http_code >= 500) return true;
    if (http_code != 400) return false;
    // 400 with a JSON error body is a real rejection (bad schema, bad model,
    // bad auth) — never retry. An empty SSE stream means the upstream died
    // before producing anything; that is transient.
    json parsed = json::parse(body, nullptr, false);
    if (!parsed.is_discarded()) return false;
    // Allow SSE comments (`: KILO PROCESSING`) and a bare [DONE]; anything
    // else (a JSON error, a data payload) is a real response.
    std::stringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == ':') continue;
        if (line == "data: [DONE]" || line == "[DONE]") continue;
        if (line.rfind("data:", 0) == 0) {
            // An empty `data:` line is ignorable; any payload is a real
            // response (the upstream said something before dying).
            std::string payload = line.substr(5);
            size_t p = payload.find_first_not_of(" \t\r");
            if (p == std::string::npos) continue;
            return false;
        }
        return false;   // unrecognized content: genuine response body
    }
    return true;
}

long read_usage_token(const json& usage, const char* key) {
    auto it = usage.find(key);
    return (it != usage.end() && it->is_number()) ? it->get<long>() : -1;
}

// Parse a single model entry into a ModelInfo. The id may come from any of
// the shapes servers use ("id", "model", "name").
ModelInfo parse_entry(const json& e) {
    ModelInfo m;
    if (e.contains("id") && e["id"].is_string())
        m.id = e["id"].get<std::string>();
    else if (e.contains("model") && e["model"].is_string())
        m.id = e["model"].get<std::string>();
    else if (e.contains("name") && e["name"].is_string())
        m.id = e["name"].get<std::string>();
    // Context window: llama.cpp reports it under meta.n_ctx; OpenAI-compatible
    // gateways (kilocode, OpenRouter, ...) advertise context_length at the top
    // level with meta absent. Prefer the llama.cpp shape when both exist.
    if (e.contains("meta") && e["meta"].is_object()) {
        const json& meta = e["meta"];
        m.context = read_int(meta, "n_ctx");
        m.context_train = read_int(meta, "n_ctx_train");
    }
    if (m.context == 0) m.context = read_int(e, "n_ctx");
    if (m.context == 0) m.context = read_int(e, "context_length");
    if (m.context_train == 0) m.context_train = read_int(e, "n_ctx_train");
    if (m.context_train == 0) m.context_train = read_int(e, "context_length");
    return m;
}

// The model array from a /v1/models body ("data" or "models" key).
const json* model_array(const json& j) {
    if (j.contains("data") && j["data"].is_array())
        return &j["data"];
    if (j.contains("models") && j["models"].is_array())
        return &j["models"];
    return nullptr;
}

// OpenAI-compatible event state machine: `data:` chunks carry
// choices[0].delta with content / reasoning_content / tool_calls.
class OpenAIStreamDecoder : public StreamDecoder {
public:
    using StreamDecoder::StreamDecoder;

protected:
    void decode_payload(const std::string& data) override {
        json evt = json::parse(data, nullptr, false);
        if (evt.is_discarded()) return;

        // The include_usage final chunk carries usage and often an empty
        // choices[].
        if (evt.contains("usage") && evt["usage"].is_object()) {
            const json& u = evt["usage"];
            if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number())
                prompt_tokens_ = u["prompt_tokens"].get<long>();
            if (u.contains("completion_tokens") &&
                u["completion_tokens"].is_number())
                completion_tokens_ = u["completion_tokens"].get<long>();
        }

        if (!evt.contains("choices") || evt["choices"].empty()) return;
        const json& delta = evt["choices"][0].value("delta", json::object());
        StreamChunk chunk;

        // Dedicated reasoning field (OpenAI/vLLM/llama.cpp reasoning models).
        for (const char* key : {"reasoning_content", "reasoning"}) {
            if (delta.contains(key) && delta[key].is_string())
                chunk.reasoning += delta[key].get<std::string>();
        }
        // Normal content, split for inline <think> tags.
        if (delta.contains("content") && delta["content"].is_string())
            segment_think(delta["content"].get<std::string>(), chunk);

        if (delta.contains("tool_calls") && !delta["tool_calls"].is_null())
            chunk.tool_calls = delta["tool_calls"];

        out_.content += chunk.delta;
        out_.reasoning += chunk.reasoning;

        if (!chunk.tool_calls.is_null()) merge_tool_fragments(chunk.tool_calls);
        emit(chunk);
    }

    void decode_end() override {
        // Compact before emitting the terminal chunk so downstream dispatch
        // only ever sees dense, name-bearing tool calls.
        drop_empty_tool_slots(out_.tool_calls);
        if (pending_.empty()) return;
        StreamChunk chunk;
        if (in_think_) chunk.reasoning = pending_;
        else chunk.delta = pending_;
        pending_.clear();
        out_.content += chunk.delta;
        out_.reasoning += chunk.reasoning;
        emit(chunk);
    }

private:
    // Inline-<think> segmentation: some models put reasoning in the normal
    // content stream wrapped in <think>...</think> rather than a separate
    // reasoning_content field. We split it out on the fly.
    void segment_think(const std::string& text, StreamChunk& chunk) {
        std::string s = pending_ + text;
        pending_.clear();
        std::size_t i = 0;
        while (i < s.size()) {
            if (!in_think_) {
                std::size_t open = s.find("<think>", i);
                if (open == std::string::npos) {
                    // Keep a short tail back in case a tag straddles the
                    // boundary.
                    std::size_t safe = s.size() > 6 ? s.size() - 6 : i;
                    if (safe < i) safe = i;
                    chunk.delta += s.substr(i, safe - i);
                    pending_ = s.substr(safe);
                    return;
                }
                chunk.delta += s.substr(i, open - i);
                i = open + 7;
                in_think_ = true;
            } else {
                std::size_t close = s.find("</think>", i);
                if (close == std::string::npos) {
                    std::size_t safe = s.size() > 7 ? s.size() - 7 : i;
                    if (safe < i) safe = i;
                    chunk.reasoning += s.substr(i, safe - i);
                    pending_ = s.substr(safe);
                    return;
                }
                chunk.reasoning += s.substr(i, close - i);
                i = close + 8;
                in_think_ = false;
            }
        }
    }

    void merge_tool_fragments(const json& frags) {
        if (frags.is_null()) return;
        if (out_.tool_calls.is_null()) out_.tool_calls = json::array();
        for (const auto& frag : frags) {
            int idx = frag.value("index", 0);
            // Sparse-index guard: a huge index must not allocate a billion
            // empty slots; out-of-range fragments are dropped.
            if (idx < 0 || idx >= kMaxToolCallsPerMessage) continue;
            while (static_cast<int>(out_.tool_calls.size()) <= idx)
                out_.tool_calls.push_back(json::object());
            json& slot = out_.tool_calls[idx];
            if (frag.contains("id") && frag["id"].is_string())
                slot["id"] = frag["id"];
            if (frag.contains("type") && frag["type"].is_string())
                slot["type"] = frag["type"];
            json& fn = slot["function"];
            if (fn.is_null()) fn = json::object();
            const json& ffn = frag.value("function", json::object());
            if (ffn.contains("name") && ffn["name"].is_string())
                fn["name"] = ffn["name"];
            if (ffn.contains("arguments"))
                accumulate_arguments(fn, ffn["arguments"]);
        }
    }

    bool in_think_ = false;  // inside an inline <think> ... </think> span
    std::string pending_;    // tail kept back for tag-boundary lookahead
};

class OpenAIDialect : public Dialect {
public:
    std::string flavor() const override { return "openai"; }

    std::string chat_url(const Config& cfg) const override {
        return cfg.api_base + "/chat/completions";
    }

    std::string models_url(const Config& cfg) const override {
        return cfg.api_base + "/models";
    }

    std::vector<std::string> auth_headers(const Config& cfg) const override {
        std::vector<std::string> headers;
        if (!cfg.api_key.empty())
            headers.push_back("Authorization: Bearer " + cfg.api_key);
        return headers;
    }

    json build_chat_body(const Config& cfg, const std::vector<Message>& messages,
                         const std::vector<std::shared_ptr<Tool>>& tools,
                         bool stream) const override {
        json body = {
            {"model", cfg.model},
            {"temperature", cfg.temperature},
            {"max_tokens", cfg.max_tokens},
            {"stream", stream},
            {"messages", json::array()}};

        // Ask the server to emit a final usage chunk during streaming so we can
        // show context usage and token counts (Qwen/llama.cpp/vLLM honour
        // this).
        if (stream) body["stream_options"] = {{"include_usage", true}};

        // Qwen-style thinking control for servers using the model's native
        // jinja chat template (llama.cpp --jinja). The template reads
        // enable_thinking (and an optional thinking_budget) from
        // chat_template_kwargs.  "auto" -> send nothing, defer to the
        // template default.
        if (cfg.thinking == "on" || cfg.thinking == "off") {
            bool enable = (cfg.thinking == "on");
            body["chat_template_kwargs"]["enable_thinking"] = enable;
            if (enable && cfg.thinking_budget > 0)
                body["chat_template_kwargs"]["thinking_budget"] =
                    cfg.thinking_budget;
        }

        // Compatibility fallback for OpenAI o-series / vLLM style reasoning
        // servers that use the reasoning_effort field instead of a jinja
        // kwarg.
        if (!cfg.reasoning_effort.empty() && cfg.reasoning_effort != "off")
            body["reasoning_effort"] = cfg.reasoning_effort;

        append_messages(body["messages"], messages);
        append_tools(body, tools);
        return body;
    }

    Message parse_completion(const std::string& response) const override {
        json resp = json::parse(response, nullptr, false);
        Message out;
        out.role = "assistant";
        if (resp.is_discarded() || !resp.contains("choices") ||
            !resp["choices"].is_array() || resp["choices"].empty()) {
            out.content =
                "[error: malformed LLM response, raw body follows]\n" +
                response;
            return out;
        }
        const json& msg = resp["choices"][0].value("message", json::object());
        out.content = strip_think(str_or_raw(msg, "content", ""));
        for (const char* key : {"reasoning_content", "reasoning"})
            out.reasoning += str_or_raw(msg, key, "");
        if (msg.contains("tool_calls") && !msg["tool_calls"].is_null()) {
            out.tool_calls = msg["tool_calls"];
            // Discard tool calls with non-JSON arguments — they poison
            // history.
            bool valid = true;
            for (const auto& tc : out.tool_calls) {
                auto fn = tc.value("function", json::object());
                std::string raw = fn.value("arguments", "");
                if (!raw.empty()) {
                    auto parsed = json::parse(raw, nullptr, false);
                    if (parsed.is_discarded()) { valid = false; break; }
                }
            }
            if (valid) {
                // Drop name-less placeholders and default `type`, same as the
                // SSE path, so junk never enters the context stack.
                out.tool_calls = sanitize_tool_calls(out.tool_calls);
                if (out.tool_calls.empty())
                    out.tool_calls = json::value_t::null;
            } else {
                out.tool_calls = json::value_t::null;
            }
        }
        return out;
    }

    std::unique_ptr<StreamDecoder> make_decoder(
        Message& out, StreamDecoder::ChunkSink on_chunk,
        std::string debug_path) const override {
        return std::make_unique<OpenAIStreamDecoder>(out, std::move(on_chunk),
                                                     std::move(debug_path));
    }

    ServerInfo parse_models_response(
        const std::string& body,
        const std::string& preferred_model) const override {
        ServerInfo info;
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded()) return info;

        const json* arr = model_array(j);
        if (!arr || arr->empty()) return info;

        // The active model's entry wins (a router may list models without
        // context metadata ahead of the one in use); otherwise the first entry
        // that reports a positive window; otherwise the first entry.
        const json* chosen = nullptr;
        if (!preferred_model.empty()) {
            for (const auto& e : *arr) {
                if (!e.is_object()) continue;
                for (const char* k : {"id", "model", "name"}) {
                    if (e.contains(k) && e[k].is_string() &&
                        e[k].get<std::string>() == preferred_model) {
                        chosen = &e;
                        break;
                    }
                }
                if (chosen) break;
            }
        }
        if (!chosen) {
            for (const auto& e : *arr)
                if (e.is_object() && parse_entry(e).context > 0) {
                    chosen = &e;
                    break;
                }
        }
        if (!chosen) chosen = &(*arr)[0];

        ModelInfo m = parse_entry(*chosen);
        info.model = m.id;
        info.context_size = m.context;
        info.context_train = m.context_train;
        info.ok = !info.model.empty() || info.context_size > 0;
        return info;
    }

    std::vector<ModelInfo> parse_model_list_response(
        const std::string& body) const override {
        std::vector<ModelInfo> out;
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded()) return out;

        const json* arr = model_array(j);
        if (!arr) return out;

        // Servers sometimes list the same model multiple times (aliases, quant
        // variants with the same id); the UI and model-set validation expect a
        // unique list.
        std::set<std::string> seen;
        for (const auto& e : *arr) {
            ModelInfo m = parse_entry(e);
            if (!m.id.empty() && seen.insert(m.id).second)
                out.push_back(std::move(m));
        }
        return out;
    }

    TokenUsage parse_usage(const std::string& raw) const override {
        TokenUsage usage;
        json resp = json::parse(raw, nullptr, false);
        if (resp.contains("usage") && resp["usage"].is_object()) {
            const json& u = resp["usage"];
            usage.prompt = read_usage_token(u, "prompt_tokens");
            usage.completion = read_usage_token(u, "completion_tokens");
        }
        return usage;
    }

    bool is_retryable(long http_code, const std::string& body) const override {
        return is_retryable_http_error(http_code, body);
    }

    int context_overflow_hint(const std::string& error_body) const override {
        return parse_context_size_from_error(error_body);
    }

private:
    void append_messages(json& out, const std::vector<Message>& messages) const {
        // Merge ALL system messages into ONE leading system message, regardless
        // of where they appear in the list. The compressed-context archive is
        // a role=system message that legitimately sits AFTER the conversation
        // turns (apply_classification appends it, then prepends the real
        // system prompt); strict GGUF chat templates (e.g. Qwen 3.6 dense)
        // reject any system message that is not at the start with HTTP 500
        // ("System message must be at the beginning"). Two passes: first
        // accumulate every system message's content, then emit the single
        // merged block before the first non-system message. Token-level KV
        // prefix caching is unaffected — the common prefix tokens are
        // identical with or without the merge.
        std::string merged_system;
        for (const auto& m : messages) {
            if (m.role != "system") continue;
            if (!merged_system.empty()) merged_system += "\n\n";
            merged_system += m.content;
        }
        bool system_emitted = false;
        for (const auto& m : messages) {
            if (m.role == "system") continue;
            // First non-system message: emit the accumulated system block
            // first so it is always at the beginning of the conversation.
            if (!system_emitted) {
                if (!merged_system.empty())
                    out.push_back({{"role", "system"}, {"content", merged_system}});
                system_emitted = true;
            }
            json jm = {{"role", m.role}};
            if (m.role == "assistant" && !m.tool_calls.is_null()) {
                // Sanitize on the way out: history restored from an old
                // session file (or assembled by an older parser) can carry
                // name-less placeholder tool_calls, which strict gateways
                // reject with a type-discriminator 400. The in-memory context
                // is untouched.
                json calls = sanitize_tool_calls(m.tool_calls);
                if (!calls.empty()) {
                    jm["tool_calls"] = std::move(calls);
                    // Some servers reject an assistant message that has
                    // tool_calls but no content field at all; emit an explicit
                    // empty string.
                    jm["content"] = m.content;
                } else {
                    // Every call was a placeholder — degrade to a plain
                    // assistant text message rather than sending an empty
                    // tool_calls array.
                    jm["content"] = m.content;
                }
            } else {
                // Every other role MUST carry a content field; an omitted
                // content yields HTTP 400 ("Assistant message must contain
                // either 'content' or 'tool_calls'"). Always emit it, even
                // when empty, so a stripped/empty assistant reply never breaks
                // the next request.
                jm["content"] = m.content;
            }
            if (m.role == "tool") {
                jm["tool_call_id"] = m.tool_call_id;
                jm["name"] = m.name;
            }
            out.push_back(jm);
        }
        // All-system message list (no turns): emit the system block.
        if (!system_emitted && !merged_system.empty())
            out.push_back({{"role", "system"}, {"content", merged_system}});
    }

    void append_tools(json& body,
                      const std::vector<std::shared_ptr<Tool>>& tools) const {
        if (tools.empty()) return;
        json tarr = json::array();
        for (const auto& t : tools) {
            json params = t->parameters_schema();
            sanitize_tool_schema(params);
            tarr.push_back({{"type", "function"},
                            {"function",
                             {{"name", t->name()},
                              {"description", t->description()},
                              {"parameters", params}}}});
        }
        body["tools"] = tarr;
        body["tool_choice"] = "auto";
    }
};

} // namespace

void sanitize_tool_schema(json& schema) {
    sanitize_node(schema);
}

std::unique_ptr<Dialect> make_openai_dialect() {
    return std::make_unique<OpenAIDialect>();
}

} // namespace agent
