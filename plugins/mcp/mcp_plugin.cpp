#include "mcp_plugin.h"

#include "agent/mcp_commands.h"
#include "agent/mcp_tools.h"
#include "agent/plugin_runtime.h"
#include "agent/registry.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

namespace agent::plugins {

namespace {

// The bar readout keeps the place it had as core UI: it drops before the
// harness's own state does.
constexpr int kMcpSegmentPriority = 1000;
constexpr int kMcpSegmentDropPriority = 8;

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        if (!out.empty())
            out += "\n";
        out += line;
    }
    return out;
}

// The /mcp verbs and their documentation, in the completions.json shape the
// command tree uses (kept out of the builder so the builder is wiring).
const char* const kMcpSubtree = R"({
        "list": {"help": "List configured MCP servers and their connection state.",
                 "man": "Lists all configured MCP servers with their transport (http/command), URL, enabled state, and connection status."},
        "show": {"help": "Show details for one MCP server.",
                 "man": "Displays a server's full configuration: transport, endpoint, enabled flag, trust level, and the tools it exposes."},
        "connect": {"help": "Connect to an MCP server and register its tools.",
                    "man": "Establishes the connection to the server and registers its tools in the agent's tool registry as mcp_<server>_<tool>."},
        "disconnect": {"help": "Disconnect from an MCP server and unregister its tools.",
                       "man": "Closes the connection and unregisters all tools that server exposed."},
        "refresh": {"help": "Reconnect all enabled MCP servers.",
                    "man": "Disconnects and reconnects every enabled server, picking up configuration changes."},
        "prompts": {"help": "List prompt templates exposed by a server.",
                    "man": "Lists the prompt templates a server advertises. Templates are invoked with '/prompt <server> <name> k=v...'."},
        "enable": {"help": "Enable a configured MCP server.",
                   "man": "Marks the server as enabled so its tools load on the next connect."},
        "disable": {"help": "Disable a configured MCP server.",
                    "man": "Marks the server as disabled; its tools are unregistered until re-enabled."},
        "trust": {"help": "Mark a server as trusted.",
                  "man": "Raises a server's trust level, affecting which of its tools run without approval."}
    })";

// Split "server rest" — the shape every /mcp verb takes.
std::pair<std::string, std::string> split_server(const std::string& arg) {
    const std::size_t sp = arg.find(' ');
    if (sp == std::string::npos)
        return {arg, ""};
    return {arg.substr(0, sp), arg.substr(sp + 1)};
}

} // namespace

void McpPlugin::shutdown() {
    stop();
}

std::vector<std::unique_ptr<Capability>> McpPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // The source is declared first, so the ledger — which unwinds in reverse —
    // tears it down last: every surface that reads the manager is gone before
    // the manager is.
    caps.push_back(std::make_unique<ToolSourceCapability>(
        "mcp_servers", [this](PluginServices& svc, ToolPort& port) { start(svc, port); },
        [this] { stop(); }));
    caps.push_back(std::make_unique<ToolCapability>("read_resource", [this](PluginServices&) {
        if (!mgr_)
            return std::vector<std::unique_ptr<Tool>>{};
        return wrap_tool(make_read_resource_tool(*mgr_));
    }));
    caps.push_back(make_tool_doc_capability("mcp_doc", tool_doc_priority::kMcp, "prompts/mcp.md"));
    caps.push_back(std::make_unique<StatusSegmentCapability>(
        "mcp", kMcpSegmentPriority, kMcpSegmentDropPriority, [this](const StatusSnapshot&) {
            if (!mgr_)
                return StatusText{};
            std::string text;
            for (const auto& st : mgr_->snapshot()) {
                if (!st.connected && st.error.empty())
                    continue;
                if (!text.empty())
                    text += "\u00b7";
                text += (st.connected ? "" : "!") + st.name;
            }
            return text.empty() ? StatusText{} : StatusText{"  mcp: " + text, StatusTone::Dim};
        }));
    caps.push_back(std::make_unique<CommandCapability>(build_mcp_spec()));
    caps.push_back(std::make_unique<CommandCapability>(build_prompt_spec()));
    caps.push_back(std::make_unique<CommandCapability>(build_get_spec()));
    return caps;
}

void McpPlugin::start(PluginServices& services, ToolPort& port) {
    port_ = &port;
    registry_ = &services.tools();
    ui_ = services.ui;
    cancel_.clear(); // a stop() request must not outlive the activation
    const std::map<std::string, McpServerConfig> servers = load_mcp_servers();
    mgr_ = std::make_unique<ServerManager>(servers, &cancel_);
    for (const auto& [name, cfg] : servers) {
        if (!cfg.enabled || !cfg.auto_connect)
            continue;
        // A failed connect is recorded on the manager's snapshot and reported
        // by /mcp list — it never takes the plugin, or the other servers, down.
        if (mgr_->connect(name).empty())
            register_server_tools(port, *registry_, *mgr_, name);
    }
}

void McpPlugin::stop() {
    port_ = nullptr;
    registry_ = nullptr;
    ui_ = nullptr;
    if (mgr_) {
        mgr_->shutdown_all();
        mgr_.reset();
    }
}

std::string McpPlugin::connect(const std::string& server) {
    if (!mgr_ || !port_ || !registry_)
        return "mcp plugin is not active";
    return mcp_connect(*mgr_, *port_, *registry_, server);
}

void McpPlugin::disconnect(const std::string& server) {
    if (!mgr_ || !port_)
        return;
    mcp_disconnect(*mgr_, *port_, server);
}

std::string McpPlugin::refresh(const std::string& server) {
    if (!mgr_ || !port_ || !registry_)
        return "mcp plugin is not active";
    return mcp_refresh(*mgr_, *port_, *registry_, server);
}

std::string McpPlugin::set_enabled(const std::string& server, bool on) {
    if (!mgr_ || !port_)
        return "mcp plugin is not active";
    return mcp_enable(*mgr_, *port_, server, on);
}

std::string McpPlugin::set_trusted(const std::string& server, bool on) {
    if (!mgr_)
        return "mcp plugin is not active";
    return mcp_trust(*mgr_, server, on);
}

std::vector<std::string> McpPlugin::list_lines() const {
    return mgr_ ? mcp_list_lines(*mgr_) : std::vector<std::string>{};
}

std::vector<std::string> McpPlugin::show_lines(const std::string& server,
                                               std::string& error) const {
    if (!mgr_) {
        error = "mcp plugin is not active";
        return {};
    }
    return mcp_show_lines(*mgr_, server, error);
}

std::string McpPlugin::prompt_text(const std::string& server, const std::string& name,
                                   const json& arguments, std::string& out_text) const {
    if (!mgr_)
        return "mcp plugin is not active";
    return mcp_prompt(*mgr_, server, name, arguments, out_text);
}

// --- Command surfaces -------------------------------------------------------
// Handlers return the text to display; the host prints it. Nothing here knows
// what a TUI is. The verbs mirror what the TUI registered as core actions
// before the feature became a plugin; the leaves that need the manager report
// "not active" rather than crashing when it is gone.

CommandSpec McpPlugin::build_mcp_spec() {
    CommandSpec spec;
    spec.root = "mcp";
    spec.help = "Manage MCP servers.";
    spec.man =
        "The mcp command manages MCP (Model Context Protocol) servers. Servers are configured "
        "globally and expose their tools to the agent as mcp_<server>_<tool>. Use '/mcp list' to "
        "see configured servers, '/mcp connect' to attach a server's tools, '/mcp trust' to "
        "approve its capabilities, and '/mcp prompts' to list its prompt templates.";
    spec.subtree = json::parse(kMcpSubtree);
    spec.handlers[""] = [this](const std::string& arg) { return root_handler(arg); };
    spec.handlers["list"] = [this](const std::string&) { return root_handler(""); };
    spec.handlers["show"] = [this](const std::string& arg) { return show_handler(arg); };
    spec.handlers["connect"] = [this](const std::string& arg) { return connect_handler(arg); };
    spec.handlers["disconnect"] = [this](const std::string& arg) {
        return disconnect_handler(arg);
    };
    spec.handlers["refresh"] = [this](const std::string& arg) { return refresh_handler(arg); };
    spec.handlers["prompts"] = [this](const std::string& arg) { return prompts_handler(arg); };
    spec.handlers["enable"] = [this](const std::string& arg) {
        return set_enabled_handler(arg, true);
    };
    spec.handlers["disable"] = [this](const std::string& arg) {
        return set_enabled_handler(arg, false);
    };
    spec.handlers["trust"] = [this](const std::string& arg) { return trust_handler(arg); };
    return spec;
}

std::string McpPlugin::servers_text() const {
    const auto lines = list_lines();
    return lines.empty() ? std::string("(no MCP servers configured \u2014 see "
                                       "~/.config/amber/mcp/<name>.conf)")
                         : join(lines);
}

std::string McpPlugin::root_handler(const std::string& rest) const {
    std::string out = servers_text();
    if (!rest.empty())
        out += "\nusage: /mcp list | show <server> | connect <server> | disconnect <server> | "
               "refresh <server> | prompts <server> | enable|disable <server> | trust <server> "
               "on|off";
    return out;
}

std::string McpPlugin::show_handler(const std::string& arg) const {
    const std::string server = split_server(arg).first;
    if (server.empty())
        return "usage: /mcp show <server>";
    std::string error;
    const auto lines = show_lines(server, error);
    return error.empty() ? join(lines) : error;
}

std::string McpPlugin::connect_handler(const std::string& arg) {
    const std::string server = split_server(arg).first;
    if (server.empty())
        return "usage: /mcp connect <server>";
    const std::string err = connect(server);
    return err.empty() ? "mcp server '" + server + "' connected" : err;
}

std::string McpPlugin::disconnect_handler(const std::string& arg) {
    const std::string server = split_server(arg).first;
    if (server.empty())
        return "usage: /mcp disconnect <server>";
    disconnect(server);
    return "mcp server '" + server + "' disconnected";
}

std::string McpPlugin::refresh_handler(const std::string& arg) {
    const std::string server = split_server(arg).first;
    if (server.empty())
        return "usage: /mcp refresh <server>";
    const std::string err = refresh(server);
    return err.empty() ? "mcp server '" + server + "' refreshed" : err;
}

std::string McpPlugin::prompts_handler(const std::string& arg) const {
    const std::string server = split_server(arg).first;
    if (server.empty())
        return "usage: /mcp prompts <server>";
    if (!mgr_)
        return "mcp plugin is not active";
    auto client = mgr_->client(server);
    if (!client)
        return "server '" + server + "' not connected";
    std::vector<std::string> lines;
    for (const auto& p : client->prompts())
        lines.push_back(server + " \u00b7 " + p.name + " \u00b7 " + p.description);
    return lines.empty() ? "server '" + server + "' exposes no prompts" : join(lines);
}

std::string McpPlugin::set_enabled_handler(const std::string& arg, bool on) {
    const std::string server = split_server(arg).first;
    const std::string verb = on ? "enable" : "disable";
    if (server.empty())
        return "usage: /mcp " + verb + " <server>";
    const std::string err = set_enabled(server, on);
    return err.empty() ? "mcp server '" + server + "' " + (on ? "enabled" : "disabled") : err;
}

std::string McpPlugin::trust_handler(const std::string& arg) {
    const auto [server, val] = split_server(arg);
    if (server.empty() || (val != "on" && val != "off"))
        return "usage: /mcp trust <server> on|off";
    const std::string err = set_trusted(server, val == "on");
    return err.empty() ? "mcp server '" + server + "' trust: " + val : err;
}

CommandSpec McpPlugin::build_prompt_spec() {
    CommandSpec spec;
    spec.root = "prompt";
    spec.help = "Invoke an MCP prompt template (fills the input line).";
    spec.man =
        "The prompt command invokes a prompt template exposed by an MCP server. The template's "
        "arguments are filled with the provided key=value pairs and the resulting prompt is "
        "placed into the input line for review before sending.";
    // No child commands: the whole invocation is the root handler's argument.
    spec.subtree = json::object();
    spec.handlers[""] = [this](const std::string& arg) {
        const auto [server, rest] = split_server(arg);
        if (server.empty())
            return std::string("usage: /prompt <server> <name> [k=v ...]");
        const auto [name, kv] = split_server(rest);
        if (name.empty())
            return std::string("usage: /prompt <server> <name> [k=v ...]");
        json arguments = json::object();
        std::size_t pos = 0;
        while (pos < kv.size()) {
            const std::size_t end = kv.find(' ', pos);
            const std::string pair = kv.substr(pos, end == std::string::npos ? end : end - pos);
            const std::size_t eq = pair.find('=');
            if (eq != std::string::npos)
                arguments[pair.substr(0, eq)] = pair.substr(eq + 1);
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        std::string text;
        // Not const: returning it by value should move, not copy.
        std::string err = prompt_text(server, name, arguments, text);
        if (!err.empty())
            return err;
        // Filling the line is the point (the [MU-02] invariant): a host with no
        // input line gets the template printed instead of a silent success.
        if (ui_ && ui_->fill_input(text))
            return "filled /prompt " + server + " " + name + " \u2014 review and send";
        return text;
    };
    return spec;
}

CommandSpec McpPlugin::build_get_spec() {
    CommandSpec spec;
    // No help/man on the root: the static `get` node already documents itself,
    // and the merge takes non-children fields from the source (an empty field
    // is not written, so the documented text survives).
    spec.root = "get";
    spec.subtree = json::parse(R"({
        "mcp": {"help": "Show MCP server status and prompts.",
                "man": "Summarizes the configured MCP servers and their exposed prompts."}
    })");
    spec.handlers["mcp"] = [this](const std::string&) {
        auto lines = list_lines();
        std::string out = lines.empty()
                              ? "(no MCP servers configured \u2014 see ~/.config/amber/mcp/"
                                "<name>.conf)"
                              : join(lines);
        if (!mgr_)
            return out;
        for (const auto& st : mgr_->snapshot()) {
            if (!st.connected)
                continue;
            auto client = mgr_->client(st.name);
            if (!client)
                continue;
            for (const auto& p : client->prompts())
                out += "\n" + st.name + " \u00b7 " + p.name + " \u00b7 " + p.description;
        }
        return out;
    };
    return spec;
}

McpPlugin* mcp_plugin_from(PluginRuntime& runtime) {
    return dynamic_cast<McpPlugin*>(runtime.find("mcp"));
}

} // namespace agent::plugins
