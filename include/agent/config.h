// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_CONFIG_H
#define AGENT_CONFIG_H

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include "agent/process.h"

namespace agent {

// Operational mode: controls tool availability, approval policy, and
// orchestration depth. Switchable at runtime via /mode.
enum class AgentMode : std::uint8_t { Read, Write, Yolo };

// Known LLM provider definitions.
struct Provider {
    std::string name;
    std::string api_base;
    std::string default_model;
    bool requires_key;
};

namespace provider {

inline const Provider openrouter  = {"openrouter",  "https://openrouter.ai/api/v1",   "openai/gpt-4o",        true};
inline const Provider kilocode   = {"kilocode",   "https://api.kilocode.ai/v1",     "kilocode/kilo-1",      true};
inline const Provider custom     = {"custom",     "",                                "",                     false};

inline const Provider* all[] = { &openrouter, &kilocode, &custom };
inline constexpr int count = 3;

inline const Provider* find(const std::string& name) {
    for (auto* p : all) if (p->name == name) return p;
    return &custom;
}

} // namespace provider

// Runtime configuration for the harness. Sourced from command-line flags,
// environment variables, and global/project config files.
// The library layer is intentionally free of any UI concerns.
struct Config {
    std::string provider_name = "custom";
    std::string api_base = "http://localhost:8000/v1";
    std::string api_key;                 // required for managed providers
    std::string model = "gpt-4o-mini";
    std::string system_prompt_path;      // markdown file
    std::string tools_prompt_path;       // markdown file advertising tools
    int max_tool_iterations = 100;
    double temperature = 0.2;
    size_t max_tokens = 16384;
    bool stream = true;                  // use SSE streaming when supported

    // Agent mode: controls tool availability and approval policy.
    //   read  — only observation tools (search, grep, read)
    //   write — all tools, approval gated
    //   yolo  — all tools, auto-approve
    AgentMode mode = AgentMode::Write;

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
    // context-usage gauge (prompt_tokens vs this). Auto-detected from the
    // server's /v1/models endpoint on startup unless set explicitly. <=0 means
    // "unknown / auto" and hides the gauge. Env: AMBER_CONTEXT.
    int context_size = 0;

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

    // Detection toggles (BitchX-style /set detection namespace).
    //   loop:      tool-loop and text-loop detectors; when off the model runs
    //              until max_tool_iterations or a natural stop.
    //   duplicate: find_duplicate_call in dispatch; when off the model may
    //              repeat the exact same tool call across turns.
    bool detection_loop = true;
    bool detection_duplicate = true;

    // Context compression settings. 0 means "use default".
    double compression_threshold = 0.0;
    int compression_min_turns = 0;
    int compression_cooldown_turns = 0;

    // Cancellation token shared by the Agent, HTTP transport, and tools.
    // Requesting cancellation (TUI Esc or /stop) sets this flag; long-running
    // operations poll it and abort cooperatively. Copies share the same flag.
    CancellationToken cancel_token;

    // Experience / memory settings. 0 means "use default".
    bool experience_enabled = true;
    int experience_max_memories = 0;
    int experience_max_skills = 0;

    // Load from a simple KEY=VALUE config file, then overlay env vars
    // (AMBER_API_BASE, AMBER_API_KEY, AMBER_MODEL, ...).
    void load(const std::string& path);
    void apply_environment();

    // Persist the user-facing settings to a KEY=VALUE file (round-trips with
    // load()). Crucially it writes *intent*, not resolved values: when a value
    // was auto-detected rather than set by the user (model_explicit /
    // context_explicit are false) it emits the auto sentinels (blank model,
    // context_size=0) so a later load() re-enables auto-detection instead of
    // pinning the last probed value. Returns false if the file can't be written.
    bool save(const std::string& path) const;

    // Apply a named provider preset (sets api_base, default_model, etc.).
    void apply_provider(const std::string& name);

    // Persist the LLM provider settings (api_base, api_key, model) to a global
    // config file. Provider settings live globally because they are not project-
    // specific. Returns false if unwritable.
    bool save_global(const std::string& path) const;

    // Persist only the project-local (non-LLM-provider) settings to a KEY=VALUE
    // file. LLM provider settings (api_base, api_key, model, context_size) are
    // intentionally omitted so they stay in the global config and are not
    // duplicated into a project-local file. Returns false if unwritable.
    bool save_settings(const std::string& path) const;

    // Validate the resolved configuration. Returns a list of human-readable
    // problems; an empty vector means the config is usable. UIs decide how to
    // surface these (abort with a message, warn, etc.). Kept UI-free here.
    std::vector<std::string> validate() const;

    std::string api_url() const noexcept { return api_base + "/chat/completions"; }
    std::string models_url() const noexcept { return api_base + "/models"; }
};

// Path to the global config file (~/.config/amber/config). Used by the CLI
// and TUI to load/store LLM provider settings across all projects.
std::string global_config_path();

// ---------------------------------------------------------------------------
// Provider storage — each provider is a key=value file under
// ~/.config/amber/providers/<name>.conf with fields:
//   name, api_base, api_key, default_model, requires_key
// ---------------------------------------------------------------------------

// Directory containing all saved provider configs.
std::string providers_dir();

// List all saved provider names (filenames without .conf extension).
std::vector<std::string> list_saved_providers();

// Load a saved provider config into a Config object. Returns false if not found.
bool load_provider(const std::string& name, Config& out);

// Save a provider config. Creates/overwrites the file.
bool save_provider(const Config& cfg);

// Delete a saved provider config. Returns false if not found.
bool delete_provider(const std::string& name);

} // namespace agent

#endif // AGENT_CONFIG_H
