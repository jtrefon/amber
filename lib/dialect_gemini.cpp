#include "agent/dialect_gemini.h"

#include "agent/agent_helpers.h"

#include <cctype>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace agent {

namespace {

constexpr const char* kRoleUser = "user";
constexpr const char* kRoleModel = "model";

// Defensive string read: response bodies are untrusted, and a throw inside a
// curl write callback is unrecoverable.
std::string str_field(const json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

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

std::string text_of_parts(const json& parts) {
    std::string out;
    if (!parts.is_array())
        return out;
    for (const auto& part : parts) {
        if (part.is_object())
            out += str_field(part, "text");
    }
    return out;
}

// Synthetic ids: the protocol has none, but the internal shape and history
// replay need a stable handle per call.
std::string synthetic_call_id(std::size_t index) {
    return "gemini_call_" + std::to_string(index);
}

// One internal message -> one or more `contents` entries.
void append_message(json& out, const Message& m, std::size_t& call_counter) {
    if (m.role == "tool") {
        json part = {
            {"functionResponse", {{"name", m.name}, {"response", {{"output", m.content}}}}}};
        out.push_back({{"role", kRoleUser}, {"parts", json::array({part})}});
        return;
    }
    if (m.role == "assistant" && m.tool_calls.is_array() && !m.tool_calls.empty()) {
        json parts = json::array();
        if (!m.content.empty())
            parts.push_back({{"text", m.content}});
        for (const auto& tc : m.tool_calls) {
            if (!tc.is_object())
                continue;
            const json& fn = tc.value("function", json::object());
            std::string name = str_field(fn, "name");
            if (name.empty())
                continue; // placeholder slots are never sent
            parts.push_back({{"functionCall",
                              {{"name", std::move(name)}, {"args", parse_arguments_object(fn)}}}});
            ++call_counter;
        }
        out.push_back({{"role", kRoleModel}, {"parts", std::move(parts)}});
        return;
    }
    const char* role = (m.role == "assistant") ? kRoleModel : kRoleUser;
    out.push_back({{"role", role}, {"parts", json::array({{{"text", m.content}}})}});
}

// A response's parts -> the internal Message.
Message message_from_parts(const json& parts, std::size_t& call_counter) {
    Message out;
    out.role = "assistant";
    if (!parts.is_array())
        return out;
    json calls = json::array();
    for (const auto& part : parts) {
        if (!part.is_object())
            continue;
        out.content += str_field(part, "text");
        auto fc = part.find("functionCall");
        if (fc == part.end() || !fc->is_object())
            continue;
        std::string name = str_field(*fc, "name");
        if (name.empty())
            continue; // unusable call: never enters history
        json args = fc->value("args", json::object());
        if (!args.is_object())
            args = json::object();
        calls.push_back({{"id", synthetic_call_id(call_counter++)},
                         {"type", "function"},
                         {"function", {{"name", std::move(name)}, {"arguments", args.dump()}}}});
    }
    if (!calls.empty())
        out.tool_calls = std::move(calls);
    return out;
}

// SSE decoder: each payload is a whole GenerateContentResponse carrying the
// parts produced so far (`alt=sse`). Parts are complete, not fragmented, so
// there is no partial-argument accumulation like OpenAI's.
class GeminiStreamDecoder : public StreamDecoder {
public:
    using StreamDecoder::StreamDecoder;

protected:
    void decode_payload(const std::string& data) override {
        json evt = json::parse(data, nullptr, false);
        if (evt.is_discarded())
            return;
        read_usage(evt.value("usageMetadata", json::object()));
        const json candidates = evt.value("candidates", json::array());
        if (!candidates.is_array() || candidates.empty())
            return;
        const json& first = candidates.front();
        if (!first.is_object())
            return;
        const json parts = first.value("content", json::object()).value("parts", json::array());
        emit_parts(parts);
    }

    void decode_end() override {
        // Nothing pending: parts arrive whole. Kept explicit because the
        // contract requires it, and a future fragmented mode lands here.
    }

private:
    void read_usage(const json& usage) {
        auto it = usage.find("promptTokenCount");
        if (it != usage.end() && it->is_number())
            prompt_tokens_ = it->get<long>();
        it = usage.find("candidatesTokenCount");
        if (it != usage.end() && it->is_number())
            completion_tokens_ = it->get<long>();
    }

    void emit_parts(const json& parts) {
        if (!parts.is_array())
            return;
        for (const auto& part : parts) {
            if (!part.is_object())
                continue;
            const std::string text = str_field(part, "text");
            if (!text.empty()) {
                StreamChunk chunk;
                chunk.delta = text;
                out_.content += text;
                emit(chunk);
                continue;
            }
            auto fc = part.find("functionCall");
            if (fc == part.end() || !fc->is_object())
                continue;
            std::string name = str_field(*fc, "name");
            if (name.empty())
                continue;
            json args = fc->value("args", json::object());
            if (!args.is_object())
                args = json::object();
            json call = {{"id", synthetic_call_id(call_count_++)},
                         {"type", "function"},
                         {"function", {{"name", std::move(name)}, {"arguments", args.dump()}}}};
            if (out_.tool_calls.is_null())
                out_.tool_calls = json::array();
            out_.tool_calls.push_back(call);
            StreamChunk chunk;
            chunk.tool_calls = json::array({std::move(call)});
            emit(chunk);
        }
    }

    // Shared with parse_completion's numbering so a streamed reply and a
    // buffered one produce the same ids for the same calls.
    std::size_t call_count_ = 0;
};

class GeminiDialect : public Dialect {
public:
    std::string flavor() const override { return "gemini"; }

    std::string chat_url(const Config& cfg) const override {
        // Streaming is a different method on this protocol (and `alt=sse` makes
        // the stream length-delimited rather than a JSON array).
        return cfg.stream ? endpoint(cfg, ":streamGenerateContent?alt=sse")
                          : endpoint(cfg, ":generateContent");
    }

    std::string models_url(const Config& cfg) const override {
        return cfg.api_base + "/v1beta/models";
    }

    std::vector<std::string> auth_headers(const Config& cfg) const override {
        std::vector<std::string> headers;
        if (!cfg.api_key.empty())
            headers.push_back("x-goog-api-key: " + cfg.api_key);
        return headers;
    }

    json build_chat_body(const Config& cfg, const std::vector<Message>& messages,
                         const std::vector<std::shared_ptr<Tool>>& tools,
                         bool stream) const override {
        json body = {{"contents", json::array()}};

        // One top-level system instruction, like the other dialects merge
        // their system turns.
        std::string system;
        for (const auto& m : messages) {
            if (m.role != "system")
                continue;
            if (!system.empty())
                system += "\n\n";
            system += m.content;
        }
        if (!system.empty())
            body["systemInstruction"] = {{"parts", json::array({{{"text", system}}})}};

        std::size_t call_counter = 0;
        for (const auto& m : messages) {
            if (m.role == "system")
                continue;
            append_message(body["contents"], m, call_counter);
        }

        body["generationConfig"] = {{"maxOutputTokens", cfg.max_tokens},
                                    {"temperature", cfg.temperature}};

        append_tools(body, tools);
        return body;
    }

    Message parse_completion(const std::string& raw) const override {
        json resp = json::parse(raw, nullptr, false);
        const json candidates =
            resp.is_discarded() ? json::array() : resp.value("candidates", json::array());
        if (!candidates.is_array() || candidates.empty()) {
            Message out;
            out.role = "assistant";
            out.content = "[error: malformed LLM response, raw body follows]\n" + raw;
            return out;
        }
        const json parts =
            candidates.front().value("content", json::object()).value("parts", json::array());
        std::size_t call_counter = 0;
        Message out = message_from_parts(parts, call_counter);
        out.content = strip_think(out.content);
        return out;
    }

    std::unique_ptr<StreamDecoder> make_decoder(Message& out, StreamDecoder::ChunkSink on_chunk,
                                                std::string debug_path) const override {
        return std::make_unique<GeminiStreamDecoder>(out, std::move(on_chunk),
                                                     std::move(debug_path));
    }

    ServerInfo parse_models_response(const std::string& body,
                                     const std::string& preferred_model) const override {
        ServerInfo info;
        for (const ModelInfo& m : parse_model_list_response(body)) {
            if (!preferred_model.empty() && m.id != preferred_model)
                continue;
            info.model = m.id;
            info.context_size = m.context;
            info.context_train = m.context_train;
            info.ok = true;
            break;
        }
        return info;
    }

    std::vector<ModelInfo> parse_model_list_response(const std::string& body) const override {
        std::vector<ModelInfo> out;
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("models") || !j["models"].is_array())
            return out;
        for (const auto& e : j["models"]) {
            if (!e.is_object())
                continue;
            ModelInfo m;
            m.id = strip_models_prefix(str_field(e, "name"));
            if (m.id.empty())
                continue;
            auto limit = e.find("inputTokenLimit");
            if (limit != e.end() && limit->is_number())
                m.context = limit->get<int>();
            out.push_back(std::move(m));
        }
        return out;
    }

    TokenUsage parse_usage(const std::string& raw) const override {
        TokenUsage usage;
        json resp = json::parse(raw, nullptr, false);
        const json u = resp.value("usageMetadata", json::object());
        if (u.contains("promptTokenCount") && u["promptTokenCount"].is_number())
            usage.prompt = u["promptTokenCount"].get<long>();
        if (u.contains("candidatesTokenCount") && u["candidatesTokenCount"].is_number())
            usage.completion = u["candidatesTokenCount"].get<long>();
        return usage;
    }

    bool is_retryable(long http_code, const std::string&) const override {
        return http_code == 429 || http_code >= 500;
    }

    int context_overflow_hint(const std::string& error_body) const override {
        // "The input token count (213456) exceeds the maximum number of
        // tokens allowed (200000)." The enforced maximum is the SECOND number;
        // the leading "exceeds the maximum number of tokens" marker keeps us
        // from reading the request's own count.
        const char* marker = "tokens allowed";
        auto pos = error_body.find(marker);
        if (pos == std::string::npos)
            return 0;
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
    std::string endpoint(const Config& cfg, const char* action) const {
        return cfg.api_base + "/v1beta/models/" + cfg.model + action;
    }

    // Listing returns "models/gemini-2.5-pro"; every other call takes the bare
    // id, so the prefix is stripped at the edge.
    static std::string strip_models_prefix(const std::string& name) {
        const std::string prefix = "models/";
        if (name.rfind(prefix, 0) == 0)
            return name.substr(prefix.size());
        return name;
    }

    void append_tools(json& body, const std::vector<std::shared_ptr<Tool>>& tools) const {
        if (tools.empty())
            return;
        json decls = json::array();
        for (const auto& t : tools) {
            decls.push_back({{"name", t->name()},
                             {"description", t->description()},
                             {"parameters", t->parameters_schema()}});
        }
        body["tools"] = json::array({{{"functionDeclarations", decls}}});
    }
};

} // namespace

std::unique_ptr<Dialect> make_gemini_dialect() {
    return std::make_unique<GeminiDialect>();
}

} // namespace agent
