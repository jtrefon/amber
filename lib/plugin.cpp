
#include "agent/plugin.h"

#include "agent/data_path.h"
#include "agent/workspace.h"

#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <regex>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <curl/curl.h>

namespace agent {

namespace {

constexpr int kProtocolVersion = 1;
constexpr int kCallTimeoutSec = 60;

std::string config_base() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return xdg;
    if (const char* home = std::getenv("HOME")) return std::string(home) + "/.config";
    return "";
}

std::string home_dir() {
    if (const char* h = std::getenv("HOME")) return h;
    return "";
}

std::string state_path() {
    return config_base() + "/amber/plugins/state.json";
}

std::string user_plugin_dir() {
    return config_base() + "/amber/plugins";
}

bool is_plugin_id(const std::string& id) {
    static const std::regex re("^[a-z0-9_]+$");
    return std::regex_match(id, re);
}

bool is_regular_file(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_executable(const std::string& p) {
    return is_regular_file(p) && access(p.c_str(), X_OK) == 0;
}

std::vector<std::string> list_subdirs(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            out.push_back(p);
    }
    closedir(d);
    return out;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

size_t download_write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* out = static_cast<std::string*>(user);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

std::string download(const std::string& url, std::string& err) {
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) {
        err = "curl init failed";
        return "";
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, download_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    CURLcode rc = curl_easy_perform(c);
    auto http = 0L;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        err = std::string("download failed: ") + curl_easy_strerror(rc);
        return "";
    }
    if (http >= 400) {
        err = "download failed: HTTP " + std::to_string(http);
        return "";
    }
    return body;
}

std::string run_capture(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), p))
        out += buf.data();
    pclose(p);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

bool PluginManager::parse_manifest(const std::string& dir, PluginManifest& out,
                                   std::string& err) {
    std::string raw = read_file(dir + "/manifest.json");
    if (raw.empty()) {
        err = "manifest.json missing or unreadable";
        return false;
    }
    json j = json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "manifest.json is not valid JSON";
        return false;
    }
    auto str = [&](const char* key) {
        return j.contains(key) && j[key].is_string()
                   ? j[key].get<std::string>()
                   : std::string();
    };
    out.id = str("id");
    out.name = str("name");
    out.version = str("version");
    out.author = str("author");
    out.url = str("url");
    out.license = str("license");
    out.description = str("description");
    out.main = str("main");
    out.protocol_version = j.value("protocol_version", 0);
    out.completion = j.contains("completion") ? j["completion"] : json::object();
    out.tools = j.contains("tools") ? j["tools"] : json::array();
    out.default_settings =
        j.contains("settings") ? j["settings"] : json::object();

    if (!is_plugin_id(out.id)) {
        err = "invalid id '" + out.id + "' (expected [a-z0-9_]+)";
        return false;
    }
    if (out.version.empty()) {
        err = "missing version";
        return false;
    }
    if (out.protocol_version != kProtocolVersion) {
        err = "protocol version " + std::to_string(out.protocol_version) +
              " unsupported (harness speaks " + std::to_string(kProtocolVersion) + ")";
        return false;
    }
    if (out.main.empty() || !is_executable(dir + "/" + out.main)) {
        err = "main executable '" + out.main + "' missing or not executable";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Discovery and state
// ---------------------------------------------------------------------------

void PluginManager::discover(const std::vector<std::string>& dirs) {
    load_state();
    std::vector<std::string> roots = dirs;
    if (roots.empty()) {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            roots.emplace_back(std::string(xdg) + "/amber/plugins");
        std::string home = home_dir();
        if (!home.empty()) {
            roots.emplace_back(home + "/.config/amber/plugins");
            roots.emplace_back(home + "/.local/share/amber/plugins");
        }
        std::string ws = Workspace::root();
        if (!ws.empty()) roots.emplace_back(ws + "/.amber/plugins");
        roots.emplace_back("/usr/local/share/amber/plugins");
        roots.emplace_back("/usr/share/amber/plugins");
    }
    plugins_.clear();
    for (const auto& root : roots) {
        for (const auto& dir : list_subdirs(root)) {
            std::string id = dir.substr(dir.find_last_of('/') + 1);
            if (find(id)) continue;
            PluginInfo info;
            info.id = id;
            info.dir = dir;
            if (!parse_manifest(dir, info.manifest, info.error)) {
                info.state = PluginState::Incompatible;
                plugins_.push_back(std::move(info));
                continue;
            }
            info.version = info.manifest.version;
            if (state_.contains(id) && state_[id].value("enabled", false))
                info.state = PluginState::Enabled;
            if (state_.contains(id) && state_[id].contains("settings"))
                info.settings = state_[id]["settings"];
            else
                info.settings = info.manifest.default_settings;
            plugins_.push_back(std::move(info));
        }
    }
}

const PluginInfo* PluginManager::find(const std::string& id) const {
    for (const auto& p : plugins_)
        if (p.id == id) return &p;
    return nullptr;
}

PluginInfo* PluginManager::find(const std::string& id) {
    for (auto& p : plugins_)
        if (p.id == id) return &p;
    return nullptr;
}

void PluginManager::load_state() {
    std::string raw = read_file(state_path());
    if (raw.empty()) return;
    state_ = json::parse(raw, nullptr, false);
    if (state_.is_discarded()) state_ = json::object();
}

void PluginManager::save_state() {
    std::error_code ec;
    std::filesystem::create_directories(state_path().substr(0, state_path().find_last_of('/')), ec);
    std::ofstream f(state_path());
    if (f) f << state_.dump(2);
}

// ---------------------------------------------------------------------------
// Session (subprocess)
// ---------------------------------------------------------------------------

void PluginManager::shutdown_session(Session& s) {
    if (s.pid > 0) {
        int status = 0;
        if (waitpid(s.pid, &status, WNOHANG) == 0) {
            std::string bye = "{\"method\":\"shutdown\"}\n";
            if (s.in_fd >= 0) {
                ssize_t wr = write(s.in_fd, bye.data(), bye.size());
                (void)wr;
            }
            for (int i = 0; i < 50; ++i) {
                if (waitpid(s.pid, &status, WNOHANG) != 0) break;
                usleep(20000);
            }
            if (waitpid(s.pid, &status, WNOHANG) == 0) kill(s.pid, SIGKILL);
            waitpid(s.pid, &status, 0);
        }
    }
    if (s.in_fd >= 0) close(s.in_fd);
    if (s.out_fd >= 0) close(s.out_fd);
    s.pid = -1;
    s.in_fd = s.out_fd = -1;
}

bool PluginManager::spawn_and_handshake(PluginInfo& info, Session& s) {
    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        info.error = "pipe failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        info.error = "fork failed";
        return false;
    }
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        execl((info.dir + "/" + info.manifest.main).c_str(),
              info.manifest.main.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    s.pid = pid;
    s.in_fd = to_child[1];
    s.out_fd = from_child[0];

    json req = {{"id", 1},
                {"method", "initialize"},
                {"params",
                 {{"protocol_version", kProtocolVersion},
                  {"settings", info.settings},
                  {"workspace", Workspace::root()}}}};
    std::string line = req.dump() + "\n";
    if (write(s.in_fd, line.data(), line.size()) != (ssize_t)line.size()) {
        info.error = "write to plugin failed";
        shutdown_session(s);
        return false;
    }
    // Read exactly one line (the initialize response).
    s.in_buf.clear();
    char buf[4096];
    while (s.in_buf.find('\n') == std::string::npos) {
        ssize_t n = read(s.out_fd, buf, sizeof buf);
        if (n <= 0) {
            info.error = "plugin exited during handshake";
            shutdown_session(s);
            return false;
        }
        s.in_buf.append(buf, (size_t)n);
    }
    size_t nl = s.in_buf.find('\n');
    std::string resp_line = s.in_buf.substr(0, nl);
    s.in_buf.erase(0, nl + 1);
    json resp = json::parse(resp_line, nullptr, false);
    if (resp.is_discarded() || !resp.contains("result") ||
        !resp["result"].value("ok", false)) {
        info.error = "initialize rejected";
        shutdown_session(s);
        return false;
    }
    return true;
}

PluginManager::Session& PluginManager::session(PluginInfo& info) {
    auto it = sessions_.find(info.id);
    if (it != sessions_.end()) return *it->second;
    auto s = std::make_unique<Session>();
    spawn_and_handshake(info, *s);
    sessions_[info.id] = std::move(s);
    return *sessions_[info.id];
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

namespace plugin_internal {

class PluginTool : public Tool {
public:
    PluginTool(PluginManager* mgr, std::string id, std::string name,
               std::string description, json schema)
        : mgr_(mgr), id_(std::move(id)), name_(std::move(name)),
          description_(std::move(description)), schema_(std::move(schema)) {}

    std::string name() const noexcept override { return "plugin_" + id_ + "_" + name_; }
    std::string description() const noexcept override { return description_; }
    json parameters_schema() const override { return schema_; }

    ToolResult execute(const json& args) const override {
        return mgr_->call_tool(*mgr_->find(id_), name_, args);
    }

private:
    PluginManager* mgr_;
    std::string id_;
    std::string name_;
    std::string description_;
    json schema_;
};

} // namespace plugin_internal

ToolResult PluginManager::call_tool(const PluginInfo& info,
                                    const std::string& name, const json& args) {
    PluginInfo* infos = nullptr;
    for (auto& p : plugins_)
        if (p.id == info.id) infos = &p;
    if (!infos) return {false, "", "plugin not found"};
    Session& s = session(*infos);
    if (s.pid < 0) return {false, "", "plugin not running: " + infos->error};

    std::scoped_lock lock(s.mtx);
    static long next_id = 2;
    json req = {{"id", next_id++},
                {"method", "tool.call"},
                {"params", {{"name", name}, {"args", args}}}};
    std::string line = req.dump() + "\n";
    if (write(s.in_fd, line.data(), line.size()) != (ssize_t)line.size())
        return {false, "", "plugin write failed (process died?)"};

    // Wait for the response line with a deadline.
    int timeout = kCallTimeoutSec * 1000;
    while (s.in_buf.find('\n') == std::string::npos) {
        struct pollfd pfd {s.out_fd, POLLIN, 0};
        int rc = poll(&pfd, 1, timeout);
        if (rc == 0) return {false, "", "plugin call timed out"};
        if (rc < 0) return {false, "", "plugin poll failed"};
        char buf[4096];
        // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
        ssize_t n = read(s.out_fd, buf, sizeof buf);
        if (n <= 0) return {false, "", "plugin exited during call"};
        s.in_buf.append(buf, (size_t)n);
    }
    size_t nl = s.in_buf.find('\n');
    std::string resp_line = s.in_buf.substr(0, nl);
    s.in_buf.erase(0, nl + 1);
    json resp = json::parse(resp_line, nullptr, false);
    if (resp.is_discarded() || !resp.contains("result"))
        return {false, "", "malformed plugin response"};
    const json& r = resp["result"];
    ToolResult out;
    out.ok = r.value("ok", false);
    out.output = r.value("output", std::string());
    if (!out.ok && out.output.empty())
        out.output = r.value("error", std::string());
    if (r.contains("meta")) out.meta = r["meta"];
    return out;
}

bool PluginManager::enable(const std::string& id, ToolRegistry& reg) {
    PluginInfo* info = find(id);
    if (!info) return false;
    if (info->state == PluginState::Incompatible) return false;
    if (info->state == PluginState::Enabled) return true;

    Session& s = session(*info);
    if (s.pid < 0) {
        info->state = PluginState::Incompatible;
        return false;
    }
    for (const auto& t : info->manifest.tools) {
        if (!t.is_object() || !t.contains("name") || !t["name"].is_string())
            continue;
        std::string name = t["name"].get<std::string>();
        std::string desc =
            t.contains("description") && t["description"].is_string()
                ? t["description"].get<std::string>()
                : std::string();
        json schema = t.contains("schema") ? t["schema"] : json::object();
        reg.register_tool(std::make_unique<plugin_internal::PluginTool>(
            this, id, name, desc, schema));
    }
    info->state = PluginState::Enabled;
    state_[id]["enabled"] = true;
    save_state();
    return true;
}

bool PluginManager::disable(const std::string& id, ToolRegistry& reg) {
    PluginInfo* info = find(id);
    if (!info) return false;
    if (info->state == PluginState::Disabled) return true;
    reg.unregister_tools_with_prefix("plugin_" + id + "_");
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        shutdown_session(*it->second);
        sessions_.erase(it);
    }
    info->state = PluginState::Disabled;
    state_[id]["enabled"] = false;
    save_state();
    return true;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

std::string PluginManager::get_setting(const std::string& id,
                                       const std::string& key) const {
    const PluginInfo* info = find(id);
    if (!info || !info->settings.contains(key)) return "";
    const json& v = info->settings[key];
    return v.is_string() ? v.get<std::string>() : v.dump();
}

bool PluginManager::set_setting(const std::string& id, const std::string& key,
                                const std::string& value) {
    PluginInfo* info = find(id);
    if (!info) return false;
    if (info->manifest.default_settings.contains(key) &&
        info->manifest.default_settings[key].is_number())
        info->settings[key] = std::strtod(value.c_str(), nullptr);
    else if (info->manifest.default_settings.contains(key) &&
             info->manifest.default_settings[key].is_boolean())
        info->settings[key] = (value == "true" || value == "1");
    else
        info->settings[key] = value;
    state_[id]["settings"] = info->settings;
    save_state();
    return true;
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

std::string PluginManager::install(const std::string& source) {
    std::string archive;
    if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
        std::string err;
        archive = download(source, err);
        if (archive.empty()) return err;
    } else {
        archive = read_file(source);
        if (archive.empty()) return "cannot read archive: " + source;
    }

    std::string tmp = std::string("/tmp/amber-plugin-install-") +
                      std::to_string(getpid());
    std::string tgz = tmp + ".tar.gz";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    {
        std::ofstream f(tgz, std::ios::binary);
        f << archive;
    }
    std::string list = run_capture("tar -tzf " + tgz + " 2>&1");
    if (list.find("manifest.json") == std::string::npos)
        return "archive does not contain manifest.json";
    std::string extract = run_capture("tar -xzf " + tgz + " -C " + tmp + " 2>&1");
    if (!extract.empty()) return "cannot unpack archive";

    // Manifest may live at the archive root or in a single top-level dir.
    std::string dir = tmp;
    PluginManifest m;
    std::string err;
    if (!parse_manifest(dir, m, err))
        dir = tmp + "/" + m.id;
    if (!parse_manifest(dir, m, err)) return err;
    std::string dest = user_plugin_dir() + "/" + m.id;
    std::filesystem::remove_all(dest, ec);
    std::error_code ec2;
    std::filesystem::create_directories(dest, ec2);
    std::string mv = run_capture("cp -a " + dir + "/. " + dest + " 2>&1");
    if (!mv.empty()) return "cannot stage plugin: " + mv;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::remove(tgz, ec);
    return "";
}

std::string PluginManager::uninstall(const std::string& id) {
    PluginInfo* info = find(id);
    if (!info) return "plugin not found: " + id;
    std::string user_dir = user_plugin_dir() + "/" + id;
    if (info->dir != user_dir)
        return "refusing to uninstall system-shipped plugin";
    state_.erase(id);
    save_state();
    std::error_code ec;
    std::filesystem::remove_all(user_dir, ec);
    return "";
}

// ---------------------------------------------------------------------------
// Advertisement
// ---------------------------------------------------------------------------

std::string plugin_tools_advertisement(const ToolRegistry& reg) {
    std::string out;
    for (const std::unique_ptr<Tool>& tool : reg.tools()) {
        if (tool->name().rfind("plugin_", 0) != 0) continue;
        out += "- `" + tool->name() + "`: " + tool->description() + "\n";
    }
    return out.empty() ? out : "## Plugins\n" + out;
}

PluginManager::~PluginManager() {
    for (auto& [id, s] : sessions_)
        shutdown_session(*s);
}

} // namespace agent
