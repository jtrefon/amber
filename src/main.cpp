
#include <agent.h>
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
#include <iostream>
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
        if (!interactive_ || spec.choices.empty()) return -1;
        std::cout << spec.title << "\n";
        for (std::size_t i = 0; i < spec.choices.size(); ++i)
            std::cout << "  " << (i + 1) << ") " << spec.choices[i] << "\n";
        std::cout << "choice [1-" << spec.choices.size() << "]: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return -1;
        try {
            const int n = std::stoi(line);
            return n >= 1 && n <= static_cast<int>(spec.choices.size()) ? n - 1 : -1;
        } catch (const std::exception&) {
            return -1;
        }
    }

    bool confirm(const agent::ConfirmSpec& spec) override {
        if (!interactive_) return false;
        std::cout << spec.title << "\n" << spec.message << "\n";
        std::cout << "proceed? [y/N]: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
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
        if (work) work();
    }

private:
    std::string ask(const agent::AskSpec& spec, bool secret) {
        if (!interactive_) return {};
        if (!spec.title.empty()) std::cout << spec.title << "\n";
        std::cout << (spec.prompt.empty() ? "value" : spec.prompt) << ": " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return {};
        if (line.empty() && !spec.initial.empty()) return spec.initial;
        (void)secret; // no echo control on a pipe; documented in the guide
        return line;
    }

    bool interactive_;
};

} // namespace

int main(int argc, char** argv) {
    agent::Config cfg;
    std::string prompt;
    std::string config_file;
    bool auto_approve = false;
    bool mcp_list_only = false;
    bool no_plugins = false;
    std::string mcp_connect_name;
    std::string mcp_prompt_server;
    std::string mcp_prompt_name;
    std::string mcp_prompt_args;

    // Global settings: LLM provider from ~/.config/amber/config
    agent::Config tmp;
    {
        std::string global_path = agent::global_config_path();
        std::ifstream gf(global_path);
        if (gf) {
            tmp.load(global_path);
            // The user's explicit context window applies regardless of the
            // provider default (the provider domain only fills it when the
            // config leaves it unknown).
            if (tmp.context_explicit && tmp.context_size > 0) {
                cfg.context_size = tmp.context_size;
                cfg.context_explicit = true;
            }
            auto providers = agent::make_default_provider_service(cfg);
            if (!tmp.provider_name.empty()) {
                auto sel = providers->select(tmp.provider_name);
                if (sel.ok())
                    agent::apply_selection(cfg, sel);
            }
        }
    }

    // First run with no config: write a commented default so there is a file
    // to edit. Never touches an existing config.
    if (config_file.empty() && agent::ensure_global_config())
        std::cerr << "info: wrote default config to " << agent::global_config_path() << "\n";

    // Load the project config by default so `amber` works without --config.
    // Explicit flags and --config override these; the file is only a base.
    {
        std::ifstream def("amber.conf");
        if (def)
            cfg.load("amber.conf");
        // A model explicitly saved to the global config (e.g. via the TUI's
        // /model set) outranks the project default, so user choices persist.
        if (!tmp.model.empty() && tmp.model_explicit) {
            cfg.model = tmp.model;
            cfg.model_explicit = true;
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            if (i + 1 < argc)
                return argv[++i];
            return def;
        };
        if (a == "--api-base")
            cfg.api_base = next("");
        else if (a == "--api-key")
            cfg.api_key = next("");
        else if (a == "--model") {
            cfg.model = next("");
            cfg.model_explicit = true;
        } else if (a == "--no-plugins")
            no_plugins = true;
        else if (a == "--system")
            cfg.system_prompt_path = next("");
        else if (a == "--tools")
            cfg.tools_prompt_path = next("");
        else if (a == "--config")
            config_file = next("");
        else if (a == "--prompt")
            prompt = next("");
        else if (a == "--yes")
            auto_approve = true;
        else if (a == "--yolo") {
            cfg.mode = agent::AgentMode::Yolo;
            auto_approve = true;
        } else if (a == "--mcp-list")
            mcp_list_only = true;
        else if (a == "--mcp-connect")
            mcp_connect_name = next("");
        else if (a == "--mcp") {
            mcp_prompt_server = next("");
            mcp_prompt_name = next("");
            for (; i + 1 < argc && argv[i + 1][0] != '-'; ++i)
                mcp_prompt_args += std::string(argv[i + 1]) + " ";
        } else if (a == "--version") {
            std::cout << "amber-cli " << agent::kVersion << " (" << agent::kBuildDate << ")\n";
            return 0;
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (prompt.empty())
            prompt = a;
        else {
            prompt += " " + a;
        }
    }

    if (!config_file.empty())
        cfg.load(config_file);
    cfg.apply_environment();

    // MCP surfaces (headless): --mcp-list, --mcp <server> <prompt> [k=v ...],
    // --mcp-connect <name>.
    if (mcp_list_only || !mcp_prompt_server.empty() || !mcp_connect_name.empty()) {
        try {
            agent::ToolRegistry mcp_registry;
            agent::ServerManager mgr(agent::load_mcp_servers(), &cfg.cancel_token);
            mgr.connect_all();
            if (!mcp_connect_name.empty()) {
                std::string err = agent::mcp_connect(mgr, mcp_registry, mcp_connect_name);
                if (!err.empty()) {
                    std::cerr << "error: " << err << "\n";
                    return 1;
                }
            }
            if (mcp_list_only) {
                for (const auto& l : agent::mcp_list_lines(mgr))
                    std::cout << l << "\n";
                mgr.shutdown_all();
                return 0;
            }
            if (!mcp_prompt_server.empty()) {
                json args = json::object();
                std::stringstream ss(mcp_prompt_args);
                std::string kv;
                while (ss >> kv) {
                    size_t eq = kv.find('=');
                    if (eq != std::string::npos && eq > 0)
                        args[kv.substr(0, eq)] = kv.substr(eq + 1);
                }
                std::string text;
                std::string err =
                    agent::mcp_prompt(mgr, mcp_prompt_server, mcp_prompt_name, args, text);
                mgr.shutdown_all();
                if (!err.empty()) {
                    std::cerr << "error: " << err << "\n";
                    return 1;
                }
                std::cout << text;
                return 0;
            }
            mgr.shutdown_all();
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
    }

    // Auto-detect model / context window from the server first, filling only
    // values the user did not set explicitly. Done before validation so a blank
    // (auto) model can be resolved from the server rather than failing.
    {
        agent::ServerInfo info = agent::apply_server_autodetect(cfg);
        if (info.ok)
            std::cerr << "[server] model=" << cfg.model << " n_ctx=" << cfg.context_size << "\n";
    }

    if (auto errs = cfg.validate(); !errs.empty()) {
        std::cerr << "error: invalid configuration:\n";
        for (const auto& e : errs)
            std::cerr << "  - " << e << "\n";
        return 2;
    }

    if (prompt.empty()) {
        std::getline(std::cin, prompt);
    }
    if (prompt.empty()) {
        std::cerr << "error: no prompt provided\n";
        return 1;
    }

    if (cfg.system_prompt_path.empty())
        cfg.system_prompt_path = agent::resolve_data_path("prompts/system.md", argv[0]);
    else
        cfg.system_prompt_path = agent::resolve_data_path(cfg.system_prompt_path, argv[0]);
    if (cfg.tools_prompt_path.empty())
        cfg.tools_prompt_path = agent::resolve_data_path("prompts/tools.md", argv[0]);
    else
        cfg.tools_prompt_path = agent::resolve_data_path(cfg.tools_prompt_path, argv[0]);

    // Fail fast: the agent cannot work without its critical data files.
    if (auto missing = agent::missing_bootstrap_files(cfg, argv[0], false); !missing.empty()) {
        std::fprintf(stderr, "error: critical data files missing:\n");
        for (const auto& m : missing)
            std::fprintf(stderr, "  - %s\n", m.c_str());
        return 2;
    }

    agent::ToolRegistry registry;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    // Tools (including the core set) are plugin contributions now: the
    // tool plugins build them from the host services below.
    subagents.set_config(cfg);
    subagents.set_parallel(cfg.subagent_parallel);
    subagents.set_max(cfg.subagent_max);

    agent::AgentHooks hooks;
    hooks.on_status = [](const std::string& s) { std::cerr << "[status] " << s << "\n"; };
    subagents.set_hooks(hooks);
    // Live streaming: surface tokens as they arrive so a long generation shows
    // progress instead of appearing to hang.
    hooks.on_token = [](const std::string& t) { std::cout << t << std::flush; };
    hooks.on_reasoning = [](const std::string& t) { std::cerr << "[think] " << t; };
    hooks.on_tool_call = [](const std::string& n, const agent::json&) {
        std::cerr << "[tool] " << n << "\n";
    };
    hooks.on_tool_result = [](const std::string& n, const agent::ToolResult& r,
                              const agent::json&) {
        std::string s = "[tool] " + n + " ";
        if (!r.ok) {
            std::string err = r.error;
            if (err.size() > 80) {
                err.resize(77);
                err += "...";
            }
            s += "\u2716 " + err;
            std::cerr << s << "\n";
            return;
        }
        // Build compact summary from meta if available
        if (r.meta.count("path") && r.meta["path"].is_string()) {
            std::string p = r.meta["path"].get<std::string>();
            long start = r.meta.value("start", 1L);
            long lines = r.meta.value("lines", 1L);
            if (p.size() > 30)
                p = "..." + p.substr(p.size() - 27);
            s += "\u2713 " + p + ":" + std::to_string(start) + "-" +
                 std::to_string(start + lines - 1) + " (" + std::to_string(lines) + " lines)";
        } else {
            int lines = 1;
            for (char c : r.output)
                if (c == '\n')
                    ++lines;
            s += "\u2713 (" + std::to_string(lines) + " lines)";
        }
        std::cerr << s << "\n";
    };
    // Approval gate for side-effecting tools (bash). With --yes, grant for the
    // session. Otherwise prompt on a TTY; if stdin is not interactive, deny
    // (fail-safe: never run shell commands unattended).
    bool tty = isatty(STDIN_FILENO);
    hooks.on_approval = [auto_approve, tty](const std::string&, const agent::json&,
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
        char c = static_cast<char>(std::tolower(line[0]));
        if (c == 'y')
            return agent::Approval::AllowOnce;
        if (c == 'a')
            return agent::Approval::AllowSession;
        if (c == 'g')
            return agent::Approval::AlwaysAllow;
        return agent::Approval::Deny;
    };
    // API-key request (HTTP 401/403 or keyless key-requiring provider).
    // Prompt on a TTY; persist to the active provider's config file so the
    // next run starts authenticated. Non-TTY headless runs fail closed
    // (never block on stdin).
    hooks.on_api_key = [&cfg, tty](const std::string& reason) -> std::string {
        if (!tty) {
            std::cerr << "[auth] " << reason
                      << "  (re-run with --api-key <key> or set it in "
                         "~/.config/amber/)\n";
            return "";
        }
        std::cerr << "\n[auth] " << reason << "\n"
                  << "  API key: " << std::flush;
        std::string key;
        if (!std::getline(std::cin, key))
            return "";
        // Trim trailing whitespace/newline.
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r' ||
                                key.back() == '\n'))
            key.pop_back();
        if (key.empty())
            return "";
        // Persist to the active provider's <name>.conf (overlays the preset
        // on the next run) and to the global config.
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

    // Build compression + experience pipeline.
    // The compressor receives the LLM client at call time (per compress()),
    // not at construction — no client reference needed here.
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto exp_cfg = agent::load_experience_config(cfg);
    auto mem_store = agent::make_memory_store(exp_cfg);
    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem_store);

    // The same plugin runtime the TUI uses: a headless run gets the same
    // bundled plugins, the same persisted state, and the same registries.
    agent::Workspace workspace;
    agent::PluginManager plugins;
    plugins.discover();
    agent::PluginRuntime plugin_runtime(registry, cfg, workspace);
    plugin_runtime.add_bundled();
    plugin_runtime.add_external(plugins);
    // Attach before activating: the tool plugins construct their tools from
    // these services, and every plugin reads the live config, which is the one
    // the whole run uses.
    agent::HostServices host_services{&jobs, &todos, &subagents, &cfg.cancel_token};
    plugin_runtime.attach_host_services(host_services);
    // A plugin's questions go to the terminal when there is one; with no TTY
    // there is nobody to ask, so they fail closed rather than block.
    CliUiServices cli_ui(::isatty(STDIN_FILENO) != 0);
    plugin_runtime.attach_ui_services(&cli_ui);
    plugin_runtime.attach_config(cfg);
    if (!no_plugins) {
        plugin_runtime.start();
    } else {
        // The core tools are plugin contributions, so skipping activation would
        // leave the agent with no tools at all. --no-plugins is meant to skip
        // the plugin tier, not to strip the harness of its built-ins.
        agent::register_default_tools(registry, jobs, todos, cfg.cancel_token, subagents,
                                      &plugin_runtime.prompts());
    }

    try {
        agent::Agent agent(cfg, registry, hooks, std::move(compressor), std::move(gate),
                           std::move(mem_store), std::move(retriever));
        agent.policy().init(agent::Workspace::local_dir() + "/policy.json");
        agent.set_events(plugin_runtime.events());
        agent.set_prompt_registry(plugin_runtime.prompts());
        std::string reply = agent.run(prompt);
        std::cout << "\n" << reply << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
