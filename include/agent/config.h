
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
    std::string git_prompt_path;         // markdown file for git workflow
    int max_tool_iterations = 100;
    long max_wall_ms = 0;        // 0 = unlimited; loop deadline (engine-enforced)
    double temperature = 0.2;
    size_t max_tokens = 16384;
    bool stream = true;                  // use SSE streaming when supported
    bool plan_tool = false;              // register todowrite tool + prompt (env AMBER_PLAN_TOOL)
    bool task_tool = false;              // register the task tool (env AMBER_TASK_TOOL)
    bool subagent_parallel = true;       // sub-agents run concurrently (env AMBER_SUBAGENT_PARALLEL)
    int subagent_max = 4;                // max concurrent sub-agents (env AMBER_SUBAGENT_MAX)

    // Agent mode: controls tool availability and approval policy.
    //   read  — only observation tools (search, grep, read); writes disallowed
    //   write — all tools available, all auto-approved within the workspace
    //   yolo  — all tools, auto-approve, full system access
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

    // Provider-level default context_size, loaded from the provider preset file
    // (~/.config/amber/providers/<name>.conf). Does NOT set context_explicit,
    // so server auto-detect and manual user overrides still win. 0 means no
    // provider-level default.
    int default_context_size = 0;

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

    // Real token usage from the last LLM call (usage.prompt_tokens), set by
    // Agent after each chat_once. Used by the compression gate instead of
    // the character-based estimate when >= 0. -1 means "not yet known".
    long prompt_tokens_used = -1;

    // Detection toggles (BitchX-style /set detection namespace).
    //   loop:      tool-loop and text-loop detectors; when off the model runs
    //              until max_tool_iterations or a natural stop. Default off.
    //   duplicate: find_duplicate_call in dispatch; when off the model may
    //              repeat the exact same tool call across turns. Default off.
    bool detection_loop = false;
    bool detection_duplicate = false;

    // Context compression settings. An unset field (explicit flag false)
    // keeps the pipeline default; an explicit 0 disables the gate.
    double compression_threshold = 0.0;
    bool compression_threshold_explicit = false;
    int compression_min_turns = 0;
    bool compression_min_turns_explicit = false;
    int compression_cooldown_turns = 0;
    bool compression_cooldown_turns_explicit = false;
    // Post-compression target as a percentage of the context window (default
    // 10): compression keeps reducing (archiving older core turns) until the
    // output is ~target_pct of context_size. Number of most-recent user
    // prompts carried over verbatim (default 10) to continue the active task.
    int compression_target_pct = 0;
    bool compression_target_pct_explicit = false;
    int compression_keep_last_prompts = 0;
    bool compression_keep_last_prompts_explicit = false;

    // Current turn counter, updated by Agent after each chat_once.
    // Used by the compression gate for cooldown tracking.
    mutable size_t turn_counter = 0;

    // Master switch for the permission/approval system in Write mode.
    // When off, gated tools run without prompting (pre-policy behavior).
    bool policy_approval = true;

    // Cancellation token shared by the Agent, HTTP transport, and tools.
    // Requesting cancellation (TUI Esc or /stop) sets this flag; long-running
    // operations poll it and abort cooperatively. Copies share the same flag.
    CancellationToken cancel_token;

    // Experience / memory settings. 0 means "use default".
    bool experience_enabled = true;
    std::string experience_store_path;
    int experience_max_memories = 0;
    int experience_max_skills = 0;
    double experience_decay_rate = 0.0;
    int experience_promote_threshold = 0;

    // Authored-skill system settings. 0 means "use default" for the budgets.
    // skills_interop gates scanning of .claude/skills and .codex/skills.
    bool skills_interop = false;
    int skills_max_discovery = 0;
    int skills_body_budget_tokens = 0;

    // Load from a simple KEY=VALUE config file, then overlay env vars
    // (AMBER_API_BASE, AMBER_API_KEY, AMBER_MODEL, ...).
    void load(const std::string& path);

    // Non-fatal parse problems encountered by load() (one bad numeric value
    // skips that key with its default, never the whole file).
    std::vector<std::string> warnings;
    void apply_environment();


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

// Directory holding global amber state (~/.config/amber, or $XDG_CONFIG_HOME/amber).
std::string global_config_dir();

// Create the global config file with commented defaults on first run so a
// fresh install has a file to edit instead of silently running bare defaults.
// Never overwrites an existing file; returns true when a file was written.
bool ensure_global_config();

} // namespace agent

#endif // AGENT_CONFIG_H
