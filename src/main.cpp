
#include <agent.h>
#include <agent/model_probe.h>
#include <agent/compressor.h>
#include <agent/experience.h>
#include <agent/data_path.h>
#include <agent/plugin_runtime.h>
#include <agent/bootstrap.h>
#include <agent/mcp_commands.h>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] [prompt]\n\n"
              << "  --api-base URL     OpenAI-compatible base URL (env AMBER_API_BASE)\n"
              << "  --api-key KEY      API key (env AMBER_API_KEY)\n"
              << "  --model NAME       Model name (env AMBER_MODEL)\n"
              << "  --no-plugins       Run without the bundled plugins\n"
              << "  --system FILE      System prompt markdown file\n"
              << "  --tools FILE       Tools advertising markdown file\n"
              << "  --config FILE      KEY=VALUE config file\n"
              << "  --prompt TEXT      User prompt (else read from stdin)\n"
              << "  --yes              Auto-approve gated tools for this session\n"
              << "  --yolo             YOLO mode: run everything with no approval gate\n"
              << "  --version          Print version and exit\n"
              << "  -h, --help         Show this help\n";
}

} // namespace

namespace {

// The CLI's user-interaction port (§8). The terminal is a line at a time, so a
// question is a prompt on stdout and an answer from stdin; with no TTY there is
// nobody to ask, so every question fails closed, exactly like the bash tool's
// approval contract.
class CliUiServices : public agent::UiServices {
public:
    explicit CliUiServices(bool interactive) : interactive_(interactive) {}

    std::string ask_text(const agent::AskSpec& spec) override {
        return ask(spec, /*secret=*/false);
    }

    std::string ask_secret(const agent::AskSpec& spec) override {
        return ask(spec, /*secret=*/true);
    }

    int choose(const agent::ChooseSpec& spec) override {
        if (!interactive_ || spec.choices.empty())
            return -1;
        std::cout << spec.title << "\n";
        for (std::size_t i = 0; i < spec.choices.size(); ++i)
            std::cout << "  " << (i + 1) << ") " << spec.choices[i] << "\n";
        std::cout << "choice [1-" << spec.choices.size() << "]: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line))
            return -1;
        try {
            const int n = std::stoi(line);
            return n >= 1 && n <= static_cast<int>(spec.choices.size()) ? n - 1 : -1;
        } catch (const std::exception&) {
            return -1;
        }
    }

    bool confirm(const agent::ConfirmSpec& spec) override {
        if (!interactive_)
            return false;
        std::cout << spec.title << "\n" << spec.message << "\n";
        std::cout << "proceed? [y/N]: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line))
            return false;
        return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
    }

    void notify(agent::UiLevel level, const std::string& message) override {
        const std::string prefix = level == agent::UiLevel::Info
                                       ? std::string()
                                       : std::string(agent::to_string(level)) + ": ";
        std::cerr << prefix << message << "\n";
    }

    // No UI thread exists here: the CLI prints as it goes, so posted work runs
    // where it was posted from. A plugin sees the same contract either way.
    void post_to_ui(std::function<void()> work) override {
        if (work)
            work();
    }

private:
    std::string ask(const agent::AskSpec& spec, bool secret) {
        if (!interactive_)
            return {};
        if (!spec.title.empty())
            std::cout << spec.title << "\n";
        std::cout << (spec.prompt.empty() ? "value" : spec.prompt) << ": " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line))
            return {};
        if (line.empty() && !spec.initial.empty())
            return spec.initial;
        (void)secret; // no echo control on a pipe; documented in the guide
        return line;
    }

    bool interactive_;
};

// --- CLI surface ------------------------------------------------------------

// What the command line asked for, beyond the Config it fills in.
struct CliArgs {
    std::string prompt;
    std::string config_file;
    bool auto_approve = false;
    bool mcp_list_only = false;
    bool no_plugins = false;
    std::string mcp_connect_name;
    std::string mcp_prompt_server;
    std::string mcp_prompt_name;
    std::string mcp_prompt_args;
};

using NextArg = std::function<std::string(const char*)>;

// Flags that consume the following argument.
bool apply_value_flag(const std::string& a, const NextArg& next, agent::Config& cfg, CliArgs& out) {
    if (a == "--api-base")
        cfg.api_base = next("");
    else if (a == "--api-key")
        cfg.api_key = next("");
    else if (a == "--model") {
        cfg.model = next("");
        cfg.model_explicit = true;
    } else if (a == "--system")
        cfg.system_prompt_path = next("");
    else if (a == "--tools")
        cfg.tools_prompt_path = next("");
    else if (a == "--config")
        out.config_file = next("");
    else if (a == "--prompt")
        out.prompt = next("");
    else if (a == "--mcp-connect")
        out.mcp_connect_name = next("");
    else
        return false;
    return true;
}

// Flags that stand alone.
bool apply_switch_flag(const std::string& a, agent::Config& cfg, CliArgs& out) {
    if (a == "--no-plugins")
        out.no_plugins = true;
    else if (a == "--yes")
        out.auto_approve = true;
    else if (a == "--yolo") {
        cfg.mode = agent::AgentMode::Yolo;
        out.auto_approve = true;
    } else if (a == "--mcp-list")
        out.mcp_list_only = true;
    else
        return false;
    return true;
}

// The trailing words of the prompt: the first positional wins and the rest are
// appended, so `amber write a file` works.
void append_prompt_word(CliArgs& out, const std::string& word) {
    if (out.prompt.empty())
        out.prompt = word;
    else
        out.prompt += " " + word;
}

// The trailing `k=v` words of a --mcp invocation, up to the next flag.
std::string collect_mcp_args(int argc, char** argv, int& i) {
    std::string out;
    while (i + 1 < argc && argv[i + 1][0] != '-')
        out += std::string(argv[++i]) + " ";
    return out;
}

// Parse the command line. Returns an exit code when a flag asked to stop
// (--version/--help), otherwise nullopt.
std::optional<int> parse_args(int argc, char** argv, agent::Config& cfg, CliArgs& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* def) -> std::string { return i + 1 < argc ? argv[++i] : def; };
        if (a == "--version") {
            std::cout << "amber-cli " << agent::kVersion << " (" << agent::kBuildDate << ")\n";
            return 0;
        }
        if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (a == "--mcp") {
            out.mcp_prompt_server = next("");
            out.mcp_prompt_name = next("");
            out.mcp_prompt_args = collect_mcp_args(argc, argv, i);
            continue;
        }
        if (apply_value_flag(a, next, cfg, out) || apply_switch_flag(a, cfg, out))
            continue;
        append_prompt_word(out, a);
    }
    return std::nullopt;
}

// The user's global config, kept so its model choice can outrank the project
// default after that file is loaded.
struct GlobalConfig {
    agent::Config cfg;
    bool loaded = false;
};

// Apply the global config's context window and provider selection. The user's
// explicit context window applies regardless of the provider default (the
// provider domain only fills it when the config leaves it unknown).
GlobalConfig apply_global_config(agent::Config& cfg) {
    GlobalConfig g;
    const std::string global_path = agent::global_config_path();
    std::ifstream gf(global_path);
    if (!gf)
        return g;
    g.cfg.load(global_path);
    g.loaded = true;
    if (g.cfg.context_explicit && g.cfg.context_size > 0) {
        cfg.context_size = g.cfg.context_size;
        cfg.context_explicit = true;
    }
    auto providers = agent::make_default_provider_service(cfg);
    if (!g.cfg.provider_name.empty()) {
        auto sel = providers->select(g.cfg.provider_name);
        if (sel.ok())
            agent::apply_selection(cfg, sel);
    }
    return g;
}

// A model explicitly saved to the global config (e.g. via the TUI's /model set)
// outranks the project default, so user choices persist.
void apply_global_model(agent::Config& cfg, const GlobalConfig& g) {
    if (!g.loaded || g.cfg.model.empty() || !g.cfg.model_explicit)
        return;
    cfg.model = g.cfg.model;
    cfg.model_explicit = true;
}

// MCP surfaces (headless): --mcp-list, --mcp <server> <prompt> [k=v ...],
// --mcp-connect <name>. Returns the exit code, or -1 when none was requested.
namespace {

// "a=1 b=2" -> {"a": "1", "b": "2"}
json parse_kv_args(const std::string& spec) {
    json out = json::object();
    std::stringstream ss(spec);
    std::string kv;
    while (ss >> kv) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && eq > 0)
            out[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return out;
}

// /mcp connect <name>: false after printing the error.
bool run_mcp_connect(agent::ServerManager& mgr, agent::ToolRegistry& reg, const CliArgs& args) {
    const std::string err = agent::mcp_connect(mgr, reg, args.mcp_connect_name);
    if (err.empty())
        return true;
    std::cerr << "error: " << err << "\n";
    return false;
}

int run_mcp_list(agent::ServerManager& mgr) {
    for (const auto& l : agent::mcp_list_lines(mgr))
        std::cout << l << "\n";
    mgr.shutdown_all();
    return 0;
}

int run_mcp_prompt(agent::ServerManager& mgr, const CliArgs& args) {
    std::string text;
    const std::string err = agent::mcp_prompt(mgr, args.mcp_prompt_server, args.mcp_prompt_name,
                                              parse_kv_args(args.mcp_prompt_args), text);
    mgr.shutdown_all();
    if (!err.empty()) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    std::cout << text;
    return 0;
}

} // namespace

int run_mcp_surfaces(agent::Config& cfg, const CliArgs& args) {
    if (!args.mcp_list_only && args.mcp_prompt_server.empty() && args.mcp_connect_name.empty())
        return -1;
    try {
        agent::ToolRegistry mcp_registry;
        agent::ServerManager mgr(agent::load_mcp_servers(), &cfg.cancel_token);
        mgr.connect_all();
        if (!args.mcp_connect_name.empty() && !run_mcp_connect(mgr, mcp_registry, args))
            return 1;
        if (args.mcp_list_only)
            return run_mcp_list(mgr);
        if (!args.mcp_prompt_server.empty())
            return run_mcp_prompt(mgr, args);
        mgr.shutdown_all();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return -1;
}

// Auto-detect model / context window, filling only values the user did not set
// explicitly. Cache-first: a fresh catalog answers from disk instantly; a stale
// or missing one revalidates here — the CLI is headless, so one bounded fetch
// per TTL is the paid round-trip.
void detect_model(agent::Config& cfg) {
    bool stale = true;
    if (auto e = agent::model_catalog_read(cfg))
        stale = !agent::model_catalog_fresh(*e);
    const agent::ServerInfo info = agent::apply_server_autodetect(cfg, stale);
    if (info.ok)
        std::cerr << "[server] model=" << cfg.model << " n_ctx=" << cfg.context_size << "\n";
}

bool validate_config(const agent::Config& cfg) {
    const auto errs = cfg.validate();
    if (errs.empty())
        return true;
    std::cerr << "error: invalid configuration:\n";
    for (const auto& e : errs)
        std::cerr << "  - " << e << "\n";
    return false;
}

// The prompt comes from --prompt, a positional argument, or stdin.
bool read_prompt(std::string& prompt) {
    if (prompt.empty())
        std::getline(std::cin, prompt);
    if (prompt.empty()) {
        std::cerr << "error: no prompt provided\n";
        return false;
    }
    return true;
}

// Resolve the prompt files next to the binary, or as the user wrote them.
void resolve_prompt_paths(agent::Config& cfg, const char* argv0) {
    cfg.system_prompt_path = agent::resolve_data_path(
        cfg.system_prompt_path.empty() ? "prompts/system.md" : cfg.system_prompt_path, argv0);
    cfg.tools_prompt_path = agent::resolve_data_path(
        cfg.tools_prompt_path.empty() ? "prompts/tools.md" : cfg.tools_prompt_path, argv0);
}

// Fail fast: the agent cannot work without its critical data files.
bool check_bootstrap_files(const agent::Config& cfg, const char* argv0) {
    const auto missing = agent::missing_bootstrap_files(cfg, argv0, false);
    if (missing.empty())
        return true;
    std::fprintf(stderr, "error: critical data files missing:\n");
    for (const auto& m : missing)
        std::fprintf(stderr, "  - %s\n", m.c_str());
    return false;
}

// --- Headless hooks ---------------------------------------------------------

void hook_status(const std::string& s) {
    std::cerr << "[status] " << s << "\n";
}

// Live streaming: surface tokens as they arrive so a long generation shows
// progress instead of appearing to hang.
void hook_token(const std::string& t) {
    std::cout << t << std::flush;
}

void hook_reasoning(const std::string& t) {
    std::cerr << "[think] " << t;
}

void hook_tool_call(const std::string& n, const agent::json&) {
    std::cerr << "[tool] " << n << "\n";
}

void hook_tool_result(const std::string& n, const agent::ToolResult& r, const agent::json&) {
    std::string s = "[tool] " + n + " ";
    if (!r.ok) {
        std::string err = r.error;
        if (err.size() > 80) {
            err.resize(77);
            err += "...";
        }
        std::cerr << s << "\u2716 " << err << "\n";
        return;
    }
    // Build a compact summary from meta when the tool reports one.
    if (r.meta.count("path") && r.meta["path"].is_string()) {
        std::string p = r.meta["path"].get<std::string>();
        const long start = r.meta.value("start", 1L);
        const long lines = r.meta.value("lines", 1L);
        if (p.size() > 30)
            p = "..." + p.substr(p.size() - 27);
        s += "\u2713 " + p + ":" + std::to_string(start) + "-" + std::to_string(start + lines - 1) +
             " (" + std::to_string(lines) + " lines)";
    } else {
        int lines = 1;
        for (char c : r.output)
            if (c == '\n')
                ++lines;
        s += "\u2713 (" + std::to_string(lines) + " lines)";
    }
    std::cerr << s << "\n";
}

// Approval gate for side-effecting tools (bash). With --yes, grant for the
// session. Otherwise prompt on a TTY; if stdin is not interactive, deny
// (fail-safe: never run shell commands unattended).
std::function<agent::Approval(const std::string&, const agent::json&, const std::string&)>
make_approval_hook(bool auto_approve, bool tty) {
    return [auto_approve, tty](const std::string&, const agent::json&,
                               const std::string& summary) -> agent::Approval {
        if (auto_approve)
            return agent::Approval::AllowSession;
        if (!tty) {
            std::cerr << "[denied] " << summary
                      << "  (approval required; re-run with --yes to allow)\n";
            return agent::Approval::Deny;
        }
        std::cerr << "\n[approval] the agent wants to " << summary << "\n"
                  << "  allow? [y]es once / [a]llow session / [g]rant always / [N]o: "
                  << std::flush;
        std::string line;
        if (!std::getline(std::cin, line) || line.empty())
            return agent::Approval::Deny;
        const char c = static_cast<char>(std::tolower(line[0]));
        if (c == 'y')
            return agent::Approval::AllowOnce;
        if (c == 'a')
            return agent::Approval::AllowSession;
        if (c == 'g')
            return agent::Approval::AlwaysAllow;
        return agent::Approval::Deny;
    };
}

// API-key request (HTTP 401/403, or a keyless key-requiring provider). Prompt on
// a TTY and persist to the active provider's config file so the next run starts
// authenticated; non-TTY headless runs fail closed (never block on stdin).
std::function<std::string(const std::string&)> make_api_key_hook(agent::Config& cfg, bool tty) {
    return [&cfg, tty](const std::string& reason) -> std::string {
        if (!tty) {
            std::cerr << "[auth] " << reason
                      << "  (re-run with --api-key <key> or set it in ~/.config/amber/)\n";
            return "";
        }
        std::cerr << "\n[auth] " << reason << "\n  API key: " << std::flush;
        std::string key;
        if (!std::getline(std::cin, key))
            return "";
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r' ||
                                key.back() == '\n'))
            key.pop_back();
        if (key.empty())
            return "";
        // Persist to the active provider's <name>.conf (overlays the preset on
        // the next run) and to the global config.
        auto providers = agent::make_default_provider_service(cfg);
        const std::string name = cfg.provider_name.empty() ? "custom" : cfg.provider_name;
        auto existing = providers->find(name);
        agent::Provider p;
        p.name = name;
        p.api_base = cfg.api_base;
        p.api_key = key;
        p.requires_key = true;
        p.default_model = cfg.model;
        p.builtin = existing ? existing->builtin : false;
        providers->save(p);
        cfg.api_key = key;
        cfg.save_global(agent::global_config_path());
        std::cerr << "[auth] key saved for provider '" << name << "'\n";
        return key;
    };
}

// --- Headless host ----------------------------------------------------------

// Everything a headless run needs to exist for its duration, in construction
// order: the plugin runtime is built from the registry, config and workspace,
// and the compression pipeline from the config.
struct CliHost {
    agent::ToolRegistry registry;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::Workspace workspace;
    agent::PluginManager plugins;
    agent::PluginRuntime plugin_runtime;
    agent::HostServices host_services;
    CliUiServices cli_ui;
    agent::CompressionConfig comp_cfg;
    std::unique_ptr<agent::CompressionGate> gate;
    std::unique_ptr<agent::CompressionStrategy> compressor;
    agent::ExperienceConfig exp_cfg;
    std::unique_ptr<agent::MemoryStore> mem_store;
    std::unique_ptr<agent::MemoryRetriever> retriever;

    CliHost(agent::Config& cfg, bool tty)
        : plugin_runtime(registry, cfg, workspace),
          host_services{&jobs, &todos, &subagents, &cfg.cancel_token}, cli_ui(tty),
          comp_cfg(agent::load_compression_config(cfg)),
          gate(agent::make_compression_gate(comp_cfg)),
          compressor(agent::make_compressor(comp_cfg)), exp_cfg(agent::load_experience_config(cfg)),
          mem_store(agent::make_memory_store(exp_cfg)),
          retriever(std::make_unique<agent::MemoryRetriever>(*mem_store)) {
        // Tools (including the core set) are plugin contributions now: the tool
        // plugins build them from the host services above.
        subagents.set_config(cfg);
        subagents.set_parallel(cfg.subagent_parallel);
        subagents.set_max(cfg.subagent_max);
    }
};

// Bring up the plugin runtime — the same one the TUI uses, so a headless run
// gets the same bundled plugins, persisted state and registries.
void start_plugin_runtime(CliHost& host, agent::Config& cfg, bool no_plugins) {
    host.plugins.discover();
    host.plugin_runtime.add_bundled();
    host.plugin_runtime.add_external(host.plugins);
    // Attach before activating: the tool plugins construct their tools from
    // these services, and every plugin reads the live config, which is the one
    // the whole run uses.
    host.plugin_runtime.attach_host_services(host.host_services);
    // A plugin's questions go to the terminal when there is one; with no TTY
    // there is nobody to ask, so they fail closed rather than block.
    host.plugin_runtime.attach_ui_services(&host.cli_ui);
    host.plugin_runtime.attach_config(cfg);
    if (!no_plugins) {
        host.plugin_runtime.start();
        return;
    }
    // The core tools are plugin contributions, so skipping activation would
    // leave the agent with no tools at all. --no-plugins is meant to skip the
    // plugin tier, not to strip the harness of its built-ins.
    agent::register_default_tools(host.registry, host.jobs, host.todos, cfg.cancel_token,
                                  host.subagents, &host.plugin_runtime.prompts());
}

// The project config is a base: explicit flags and --config override it, and a
// model saved to the global config outranks both.
void load_run_config(agent::Config& cfg, const CliArgs& args, const GlobalConfig& global) {
    std::ifstream def("amber.conf");
    if (def)
        cfg.load("amber.conf");
    apply_global_model(cfg, global);
    if (!args.config_file.empty())
        cfg.load(args.config_file);
    cfg.apply_environment();
}

// The headless hooks, installed in the order the executor needs: set_hooks
// copies, so the sub-agent executor sees the status hook only.
agent::AgentHooks apply_headless_hooks(CliHost& host, agent::Config& cfg, bool auto_approve,
                                       bool tty) {
    agent::AgentHooks hooks;
    hooks.on_status = hook_status;
    host.subagents.set_hooks(hooks);
    hooks.on_token = hook_token;
    hooks.on_reasoning = hook_reasoning;
    hooks.on_tool_call = hook_tool_call;
    hooks.on_tool_result = hook_tool_result;
    hooks.on_approval = make_approval_hook(auto_approve, tty);
    hooks.on_api_key = make_api_key_hook(cfg, tty);
    return hooks;
}

// One headless run: the agent gets the plugin runtime's events and prompt
// registry, streams its reply to stdout, and reports failure as an exit code.
int run_agent_once(agent::Config& cfg, CliHost& host, const agent::AgentHooks& hooks,
                   const std::string& prompt) {
    try {
        agent::Agent agent(cfg, host.registry, hooks, std::move(host.compressor),
                           std::move(host.gate), std::move(host.mem_store),
                           std::move(host.retriever));
        agent.policy().init(agent::Workspace::local_dir() + "/policy.json");
        agent.set_events(host.plugin_runtime.events());
        agent.set_prompt_registry(host.plugin_runtime.prompts());
        std::cout << "\n" << agent.run(prompt) << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) try {
    agent::Config cfg;
    CliArgs args;

    const GlobalConfig global = apply_global_config(cfg);

    // First run with no config: write a commented default so there is a file to
    // edit. Never touches an existing config.
    if (args.config_file.empty() && agent::ensure_global_config())
        std::cerr << "info: wrote default config to " << agent::global_config_path() << "\n";

    if (std::optional<int> rc = parse_args(argc, argv, cfg, args))
        return *rc;
    load_run_config(cfg, args, global);

    if (const int rc = run_mcp_surfaces(cfg, args); rc >= 0)
        return rc;

    detect_model(cfg);
    if (!validate_config(cfg))
        return 2;
    if (!read_prompt(args.prompt))
        return 1;
    resolve_prompt_paths(cfg, argv[0]);
    if (!check_bootstrap_files(cfg, argv[0]))
        return 2;

    const bool tty = isatty(STDIN_FILENO) != 0;
    CliHost host(cfg, tty);
    const agent::AgentHooks hooks = apply_headless_hooks(host, cfg, args.auto_approve, tty);
    start_plugin_runtime(host, cfg, args.no_plugins);
    return run_agent_once(cfg, host, hooks, args.prompt);
} catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
}
