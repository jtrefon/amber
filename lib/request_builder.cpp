
#include "agent/request_builder.h"

namespace agent {

namespace {

auto read_int = [](const json& o, const char* k) -> int {
    auto it = o.find(k);
    return (it != o.end() && it->is_number_integer()) ? it->get<int>() : 0;
};

// Repair a tool parameters_schema so the server's grammar builder never sees
// null types or arrays without items (llama.cpp 400s with "type must be
// array, but is null"). Applied to every tool before the request is sent.
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

} // namespace

void sanitize_tool_schema(json& schema) {
    sanitize_node(schema);
}

json build_chat_body(const Config& cfg, const std::vector<Message>& messages,
                     const std::vector<std::shared_ptr<Tool>>& tools, bool stream) {
    json body = {
        {"model", cfg.model},
        {"temperature", cfg.temperature},
        {"max_tokens", cfg.max_tokens},
        {"stream", stream},
        {"messages", json::array()}};

    // Ask the server to emit a final usage chunk during streaming so we can show
    // context usage and token counts (Qwen/llama.cpp/vLLM honour this).
    if (stream) body["stream_options"] = {{"include_usage", true}};

    // Qwen-style thinking control for servers using the model's native jinja
    // chat template (llama.cpp --jinja). The template reads enable_thinking (and
    // an optional thinking_budget) from chat_template_kwargs.
    //   "auto" -> send nothing, defer to the template default.
    if (cfg.thinking == "on" || cfg.thinking == "off") {
        bool enable = (cfg.thinking == "on");
        body["chat_template_kwargs"]["enable_thinking"] = enable;
        if (enable && cfg.thinking_budget > 0)
            body["chat_template_kwargs"]["thinking_budget"] =
                cfg.thinking_budget;
    }

    // Compatibility fallback for OpenAI o-series / vLLM style reasoning servers
    // that use the reasoning_effort field instead of a jinja kwarg.
    if (!cfg.reasoning_effort.empty() && cfg.reasoning_effort != "off")
        body["reasoning_effort"] = cfg.reasoning_effort;

    // Merge consecutive system messages into one. The memory/skills blocks are
    // injected as a second role=system message; strict GGUF chat templates
    // (e.g. Qwen 3.6 dense) reject multiple system messages with HTTP 500.
    // Token-level KV prefix caching is unaffected — the common prefix tokens
    // are identical with or without the merge.
    std::string merged_content;
    bool system_open = false;
    for (const auto& m : messages) {
        if (m.role == "system") {
            if (!system_open) {
                merged_content = m.content;
                system_open = true;
            } else {
                merged_content += "\n\n" + m.content;
            }
            continue;
        }
        if (system_open) {
            body["messages"].push_back(
                {{"role", "system"}, {"content", merged_content}});
            system_open = false;
        }
        json jm = {{"role", m.role}};
        if (m.role == "assistant" && !m.tool_calls.is_null()) {
            jm["tool_calls"] = m.tool_calls;
            // Some servers reject an assistant message that has tool_calls but
            // no content field at all; emit an explicit empty string.
            if (m.content.empty()) jm["content"] = "";
            else jm["content"] = m.content;
        } else {
            // Every other role MUST carry a content field; an omitted content
            // yields HTTP 400 ("Assistant message must contain either
            // 'content' or 'tool_calls'"). Always emit it, even when empty, so
            // a stripped/empty assistant reply never breaks the next request.
            jm["content"] = m.content;
        }
        if (m.role == "tool") {
            jm["tool_call_id"] = m.tool_call_id;
            jm["name"] = m.name;
        }
        body["messages"].push_back(jm);
    }
    if (system_open)
        body["messages"].push_back(
            {{"role", "system"}, {"content", merged_content}});

    if (!tools.empty()) {
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
    return body;
}

} // namespace agent
