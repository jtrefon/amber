// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_CONFIG_H
#define AGENT_CONFIG_H

#include <string>
#include <map>
#include <vector>

namespace agent {

// Runtime configuration for the harness. Sourced from command-line flags,
// environment variables, and an optional config file. The library layer is
// intentionally free of any UI concerns.
struct Config {
    std::string api_base = "http://localhost:8000/v1";
    std::string api_key;                 // optional for local endpoints
    std::string model = "gpt-4o-mini";
    std::string system_prompt_path;      // markdown file
    std::string tools_prompt_path;       // markdown file advertising tools
    int max_tool_iterations = 32;
    double temperature = 0.2;
    size_t max_tokens = 4096;
    bool stream = true;                  // use SSE streaming when supported

    // Thinking / reasoning control for Qwen-style models served with a native
    // jinja chat template (llama.cpp --jinja). The template exposes an
    // enable_thinking kwarg (and an optional thinking_budget token cap) which we
    // pass through chat_template_kwargs.
    //   thinking: "on" | "off" | "auto"
    //     on   -> enable_thinking = true
    //     off  -> enable_thinking = false
    //     auto -> send nothing, let the template/server decide
    std::string thinking = "auto";
    // Soft cap on thinking tokens; <=0 means "unset" (no thinking_budget sent).
    int thinking_budget = -1;
    bool show_reasoning = true;          // render thinking live in the UI

    // Model context window (n_ctx) in tokens. Used by UIs to render a
    // context-usage gauge (prompt_tokens vs this). May be auto-detected from the
    // server's /v1/models endpoint on startup. <=0 hides the gauge.
    // Env: AMBER_CONTEXT.
    int context_size = 8192;

    // Set true when model / context_size were provided explicitly (config file,
    // env, or CLI flag). Startup auto-detection only fills values that were NOT
    // set explicitly, so the user always wins.
    bool model_explicit = false;
    bool context_explicit = false;

    // Compatibility fallback for OpenAI o-series / vLLM style servers that use
    // the reasoning_effort field instead of a jinja kwarg: "off" disables it.
    std::string reasoning_effort = "off";

    // Conversation / telemetry log. When non-empty, the agent appends one JSON
    // object per event (JSON Lines) to this path. Supports a literal "{ts}"
    // placeholder, expanded to a start-of-session unix timestamp. Empty = off.
    std::string log_path;

    // Debug log. When non-empty, LLMClient dumps raw HTTP request bodies, raw
    // SSE bytes, HTTP status, and any transport/parse errors here. Verbose;
    // intended for diagnosing streaming/crash issues. Supports "{ts}". Off when
    // empty. Env: AMBER_DEBUG.
    std::string debug_log;

    // Load from a simple KEY=VALUE config file, then overlay env vars
    // (AMBER_API_BASE, AMBER_API_KEY, AMBER_MODEL, ...).
    void load(const std::string& path);
    void apply_environment();

    // Validate the resolved configuration. Returns a list of human-readable
    // problems; an empty vector means the config is usable. UIs decide how to
    // surface these (abort with a message, warn, etc.). Kept UI-free here.
    std::vector<std::string> validate() const;

    std::string api_url() const { return api_base + "/chat/completions"; }
    std::string models_url() const { return api_base + "/models"; }
};

} // namespace agent

#endif // AGENT_CONFIG_H
