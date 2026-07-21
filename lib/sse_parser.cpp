// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/sse_parser.h"
#include "agent/debug_log.h"
#include <nlohmann/json.hpp>

namespace agent {

namespace {

// Inline-<think> segmentation: some models put reasoning in the normal content
// stream wrapped in <think>...</think> rather than a separate reasoning_content
// field. We split it out on the fly.
void segment_think_impl(SseState& st, const std::string& text,
                        StreamChunk& chunk) {
    std::string s = st.pending + text;
    st.pending.clear();
    size_t i = 0;
    while (i < s.size()) {
        if (!st.in_think) {
            size_t open = s.find("<think>", i);
            if (open == std::string::npos) {
                // Keep a short tail back in case a tag straddles the boundary.
                size_t safe = s.size() > 6 ? s.size() - 6 : i;
                if (safe < i) safe = i;
                chunk.delta += s.substr(i, safe - i);
                st.pending = s.substr(safe);
                return;
            }
            chunk.delta += s.substr(i, open - i);
            i = open + 7;
            st.in_think = true;
        } else {
            size_t close = s.find("</think>", i);
            if (close == std::string::npos) {
                size_t safe = s.size() > 7 ? s.size() - 7 : i;
                if (safe < i) safe = i;
                chunk.reasoning += s.substr(i, safe - i);
                st.pending = s.substr(safe);
                return;
            }
            chunk.reasoning += s.substr(i, close - i);
            i = close + 8;
            st.in_think = false;
        }
    }
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

void finalize_impl(SseState& st, Message& out,
                   const StreamParser::ChunkSink& on_chunk) {
    if (st.finished) return;
    st.finished = true;
    if (!st.pending.empty()) {
        StreamChunk chunk;
        if (st.in_think) chunk.reasoning = st.pending;
        else chunk.delta = st.pending;
        st.pending.clear();
        out.content += chunk.delta;
        out.reasoning += chunk.reasoning;
        if (on_chunk && (!chunk.delta.empty() || !chunk.reasoning.empty()))
            on_chunk(chunk);
    }
    if (on_chunk) {
        StreamChunk end;
        end.done = true;
        on_chunk(end);
    }
}

void dispatch_event_impl(SseState& st, Message& out,
                         const StreamParser::ChunkSink& on_chunk,
                         const std::string& data) {
    if (data == "[DONE]") {
        finalize_impl(st, out, on_chunk);
        return;
    }
    json evt = json::parse(data, nullptr, false);
    if (evt.is_discarded()) return;

    // The include_usage final chunk carries usage and often an empty choices[].
    if (evt.contains("usage") && evt["usage"].is_object()) {
        const json& u = evt["usage"];
        if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number())
            st.prompt_tokens = u["prompt_tokens"].get<long>();
        if (u.contains("completion_tokens") && u["completion_tokens"].is_number())
            st.completion_tokens = u["completion_tokens"].get<long>();
    }

    if (!evt.contains("choices") || evt["choices"].empty())
        return;
    const json& delta = evt["choices"][0].value("delta", json::object());
    StreamChunk chunk;

    // Dedicated reasoning field (OpenAI/vLLM/llama.cpp reasoning models).
    for (const char* key : {"reasoning_content", "reasoning"}) {
        if (delta.contains(key) && delta[key].is_string())
            chunk.reasoning += delta[key].get<std::string>();
    }
    // Normal content, split for inline <think> tags.
    if (delta.contains("content") && delta["content"].is_string())
        segment_think_impl(st, delta["content"].get<std::string>(), chunk);

    if (delta.contains("tool_calls") && !delta["tool_calls"].is_null())
        chunk.tool_calls = delta["tool_calls"];

    out.content += chunk.delta;
    out.reasoning += chunk.reasoning;

    if (!chunk.tool_calls.is_null()) {
        if (out.tool_calls.is_null()) out.tool_calls = json::array();
        for (const auto& frag : chunk.tool_calls) {
            int idx = frag.value("index", 0);
            while (static_cast<int>(out.tool_calls.size()) <= idx)
                out.tool_calls.push_back(json::object());
            json& slot = out.tool_calls[idx];
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
    if (on_chunk && (!chunk.delta.empty() || !chunk.reasoning.empty() ||
                     !chunk.tool_calls.is_null()))
        on_chunk(chunk);
}

} // namespace

size_t StreamParser::on_write(const char* data, size_t size, size_t nmemb) {
    const std::string raw(data, size * nmemb);
    if (!debug_path_.empty())
        debug_log(debug_path_, "sse-raw", raw);
    st_.buffer.append(raw);

    size_t nl;
    while ((nl = st_.buffer.find('\n')) != std::string::npos) {
        std::string line = st_.buffer.substr(0, nl);
        st_.buffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("data:", 0) != 0) continue;
        std::string payload = line.substr(5);
        size_t p = payload.find_first_not_of(" \t");
        if (p != std::string::npos) payload = payload.substr(p);
        dispatch_event_impl(st_, out_, on_chunk_, payload);
    }
    return size * nmemb;
}

void StreamParser::finalize() {
    finalize_impl(st_, out_, on_chunk_);
}

} // namespace agent
