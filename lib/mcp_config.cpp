
#include "agent/mcp_config.h"
#include "agent/config.h"
#include "agent/mcp_transport_http.h"
#include "agent/mcp_transport_stdio.h"
#include "agent/workspace.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace agent {

namespace {

std::string mcp_dir(bool project) {
    return project ? Workspace::local_dir() + "/mcp"
                   : global_config_dir() + "/mcp";
}

bool parse_bool(const std::string& val) {
    return val == "1" || val == "true" || val == "yes";
}

std::vector<std::string> split_args(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// Apply one KEY=VALUE line to the config under construction.
void apply_field(McpServerConfig& cfg, const std::string& key,
                 const std::string& val) {
    if (key == "type") cfg.type = val;
    else if (key == "command") cfg.command = val;
    else if (key == "args") cfg.args = split_args(val);
    else if (key == "cwd") cfg.cwd = val;
    else if (key == "url") cfg.url = val;
    else if (key == "auth_token") cfg.auth_token = val;
    else if (key == "enabled") cfg.enabled = parse_bool(val);
    else if (key == "auto_connect") cfg.auto_connect = parse_bool(val);
    else if (key == "trusted") cfg.trusted = parse_bool(val);
    else if (key == "timeout_s") cfg.timeout_s = std::stoi(val);
}

void validate(McpServerConfig& cfg) {
    cfg.error.clear();
    if (cfg.type != "stdio" && cfg.type != "http") {
        cfg.error = "invalid type '" + cfg.type + "' (stdio|http)";
    } else if (cfg.type == "stdio" && cfg.command.empty()) {
        cfg.error = "stdio server requires 'command'";
    } else if (cfg.type == "http" && cfg.url.empty()) {
        cfg.error = "http server requires 'url'";
    }
    if (cfg.timeout_s <= 0) cfg.timeout_s = 60;
}

McpServerConfig parse_file(const std::string& path) {
    McpServerConfig cfg;
    cfg.name = fs::path(path).stem().string();
    std::ifstream f(path);
    if (!f.is_open()) {
        cfg.error = "unreadable config";
        return cfg;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        apply_field(cfg, line.substr(0, eq), line.substr(eq + 1));
    }
    validate(cfg);
    return cfg;
}

// Overlay one file's fields onto an existing config (project over global);
// absent keys keep their global values.
void overlay_file(const std::string& path, McpServerConfig& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        apply_field(cfg, line.substr(0, eq), line.substr(eq + 1));
    }
    validate(cfg);
}

// Load one directory's configs into `out`; `overlay` merges fields onto
// existing entries (project wins per key), otherwise entries are replaced.
void load_dir(const std::string& dir, bool overlay,
              std::map<std::string, McpServerConfig>& out) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::error_code e2;
        if (!entry.is_regular_file(e2)) continue;
        if (entry.path().extension() != ".conf") continue;
        std::string name = entry.path().stem().string();
        if (name.empty()) continue;
        auto it = out.find(name);
        if (overlay && it != out.end()) {
            overlay_file(entry.path().string(), it->second);
        } else {
            out[name] = parse_file(entry.path().string());
        }
    }
}

} // namespace

std::map<std::string, McpServerConfig> load_mcp_servers() {
    std::map<std::string, McpServerConfig> out;
    load_dir(mcp_dir(false), false, out);
    load_dir(mcp_dir(true), true, out);

    const char* env = std::getenv("AMBER_MCP_SERVERS");
    if (env && *env) {
        std::stringstream ss(env);
        std::string name;
        std::vector<std::string> enabled_names;
        while (std::getline(ss, name, ',')) {
            if (!name.empty()) enabled_names.push_back(name);
        }
        for (auto& kv : out)
            kv.second.enabled =
                std::find(enabled_names.begin(), enabled_names.end(),
                          kv.first) != enabled_names.end();
    }
    return out;
}

bool save_mcp_server(const McpServerConfig& cfg) {
    std::error_code ec;
    fs::path dir = mcp_dir(true);
    fs::create_directories(dir, ec);
    if (ec) return false;
    std::ofstream f(dir / (cfg.name + ".conf"), std::ios::trunc);
    if (!f) return false;
    f << "type=" << cfg.type << "\n";
    if (!cfg.command.empty()) f << "command=" << cfg.command << "\n";
    if (!cfg.args.empty()) {
        f << "args=";
        for (size_t i = 0; i < cfg.args.size(); ++i) {
            if (i) f << " ";
            f << cfg.args[i];
        }
        f << "\n";
    }
    fs::permissions(dir / (cfg.name + ".conf"), fs::perms::owner_read |
                                                    fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    if (!cfg.cwd.empty()) f << "cwd=" << cfg.cwd << "\n";
    if (!cfg.url.empty()) f << "url=" << cfg.url << "\n";
    if (!cfg.auth_token.empty()) f << "auth_token=" << cfg.auth_token << "\n";
    f << "enabled=" << (cfg.enabled ? 1 : 0) << "\n";
    f << "auto_connect=" << (cfg.auto_connect ? 1 : 0) << "\n";
    f << "trusted=" << (cfg.trusted ? 1 : 0) << "\n";
    f << "timeout_s=" << cfg.timeout_s << "\n";
    return static_cast<bool>(f);
}

bool delete_mcp_server(const std::string& name) {
    std::error_code ec;
    return fs::remove(fs::path(mcp_dir(true)) / (name + ".conf"), ec);
}

ServerManager::ServerManager(std::map<std::string, McpServerConfig> servers,
                             const CancellationToken* cancel_token)
    : configs_(std::move(servers)), cancel_token_(cancel_token) {}

void ServerManager::connect_all() {
    for (const auto& kv : configs_) {
        if (kv.second.enabled && kv.second.auto_connect) connect(kv.first);
    }
}

std::string ServerManager::connect(const std::string& name) {
    auto it = configs_.find(name);
    if (it == configs_.end()) return "unknown server '" + name + "'";
    if (!it->second.enabled) return "server '" + name + "' is disabled";
    if (!it->second.error.empty()) return it->second.error;

    disconnect(name);
    const McpServerConfig& cfg = it->second;
    std::unique_ptr<McpTransport> transport;
    std::string transport_error;
    int timeout_ms = cfg.timeout_s * 1000;
    if (cfg.type == "stdio") {
        std::string cwd = cfg.cwd.empty() ? Workspace::root() : cfg.cwd;
        transport = std::make_unique<StdioTransport>(cfg.command, cfg.args,
                                                     cwd, nullptr,
                                                     timeout_ms);
    } else {
        transport = std::make_unique<HttpTransport>(cfg.url, cfg.auth_token,
                                                    timeout_ms, nullptr);
    }
    auto client = std::make_unique<MCPClient>(name, std::move(transport),
                                              transport_error, cancel_token_);
    std::string err = client->connect();
    clients_[name] = std::move(client);
    return err;
}

void ServerManager::disconnect(const std::string& name) {
    auto it = clients_.find(name);
    if (it == clients_.end()) return;
    it->second->disconnect();
    clients_.erase(it);
}

std::string ServerManager::refresh(const std::string& name) {
    auto it = clients_.find(name);
    if (it == clients_.end()) return "server '" + name + "' not connected";
    return it->second->refresh();
}

std::string ServerManager::set_trusted(const std::string& name, bool trusted) {
    auto it = configs_.find(name);
    if (it == configs_.end()) return "unknown server '" + name + "'";
    it->second.trusted = trusted;
    if (!save_mcp_server(it->second)) return "could not save config for '" +
                                                 name + "'";
    return "";
}

std::string ServerManager::set_enabled(const std::string& name, bool enabled) {
    auto it = configs_.find(name);
    if (it == configs_.end()) return "unknown server '" + name + "'";
    it->second.enabled = enabled;
    if (!enabled) disconnect(name);
    if (!save_mcp_server(it->second)) return "could not save config for '" +
                                                 name + "'";
    return "";
}

std::string ServerManager::add_server(McpServerConfig cfg) {
    validate(cfg);
    if (cfg.name.empty()) return "missing server name";
    if (!cfg.error.empty()) return cfg.error;
    configs_[cfg.name] = cfg;
    if (!save_mcp_server(cfg)) return "could not save config for '" +
                                          cfg.name + "'";
    return "";
}

std::string ServerManager::remove_server(const std::string& name) {
    if (configs_.erase(name) == 0) return "unknown server '" + name + "'";
    disconnect(name);
    delete_mcp_server(name);
    return "";
}

std::vector<McpServerStatus> ServerManager::snapshot() const {
    std::vector<McpServerStatus> out;
    for (const auto& kv : configs_) {
        McpServerStatus st;
        st.name = kv.first;
        st.type = kv.second.type;
        st.enabled = kv.second.enabled;
        st.trusted = kv.second.trusted;
        st.error = kv.second.error;
        auto it = clients_.find(kv.first);
        if (it != clients_.end()) {
            if (!it->second->error().empty()) st.error = it->second->error();
            if (it->second->connected()) {
                st.connected = true;
                st.tool_count = static_cast<int>(it->second->tools().size());
                st.resource_count =
                    static_cast<int>(it->second->resources().size());
                st.prompt_count =
                    static_cast<int>(it->second->prompts().size());
            }
        }
        out.push_back(std::move(st));
    }
    return out;
}

bool ServerManager::has_server(const std::string& name) const {
    return configs_.count(name) > 0;
}

bool ServerManager::trusted(const std::string& name) const {
    auto it = configs_.find(name);
    return it != configs_.end() && it->second.trusted;
}

bool ServerManager::enabled(const std::string& name) const {
    auto it = configs_.find(name);
    return it != configs_.end() && it->second.enabled;
}

std::shared_ptr<MCPClient> ServerManager::client(const std::string& name) const {
    auto it = clients_.find(name);
    return it == clients_.end() ? nullptr : it->second;
}

void ServerManager::shutdown_all() {
    for (auto& kv : clients_) kv.second->disconnect();
    clients_.clear();
}

} // namespace agent
