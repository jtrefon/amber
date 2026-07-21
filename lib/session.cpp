// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/session.h"
#include "agent/workspace.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

namespace agent {

namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

// Serialize a single conversation Message to JSON (round-trips all fields the
// agent loop depends on, including tool call plumbing).
json msg_to_json(const Message& m) {
    json j;
    j["role"] = m.role;
    j["content"] = m.content;
    if (!m.reasoning.empty()) j["reasoning"] = m.reasoning;
    if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
    if (!m.name.empty()) j["name"] = m.name;
    if (!m.tool_calls.is_null()) j["tool_calls"] = m.tool_calls;
    return j;
}

Message msg_from_json(const json& j) {
    Message m;
    if (j.contains("role") && j["role"].is_string())
        m.role = j["role"].get<std::string>();
    if (j.contains("content") && j["content"].is_string())
        m.content = j["content"].get<std::string>();
    if (j.contains("reasoning") && j["reasoning"].is_string())
        m.reasoning = j["reasoning"].get<std::string>();
    if (j.contains("tool_call_id") && j["tool_call_id"].is_string())
        m.tool_call_id = j["tool_call_id"].get<std::string>();
    if (j.contains("name") && j["name"].is_string())
        m.name = j["name"].get<std::string>();
    if (j.contains("tool_calls")) m.tool_calls = j["tool_calls"];
    return m;
}

std::string default_dir() {
    // Project-local sessions live under "<workspace>/.amber/sessions" so all
    // amber data stays next to the project instead of scattering into $XDG.
    return agent::Workspace::local_dir() + "/sessions";
}

// mkdir -p for a path.
bool mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/" || cur.empty()) continue;
            std::string d = cur;
            if (d.back() == '/') d.pop_back();
            if (::mkdir(d.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
    }
    return true;
}

} // namespace

json Session::to_json() const {
    json arr = json::array();
    for (const auto& m : messages) arr.push_back(msg_to_json(m));
    return json{
        {"id", id},
        {"title", title},
        {"model", model},
        {"created_ms", created_ms},
        {"updated_ms", updated_ms},
        {"messages", arr},
    };
}

Session Session::from_json(const json& j) {
    Session s;
    if (j.contains("id") && j["id"].is_string()) s.id = j["id"].get<std::string>();
    if (j.contains("title") && j["title"].is_string())
        s.title = j["title"].get<std::string>();
    if (j.contains("model") && j["model"].is_string())
        s.model = j["model"].get<std::string>();
    if (j.contains("created_ms") && j["created_ms"].is_number_integer())
        s.created_ms = j["created_ms"].get<long long>();
    if (j.contains("updated_ms") && j["updated_ms"].is_number_integer())
        s.updated_ms = j["updated_ms"].get<long long>();
    if (j.contains("messages") && j["messages"].is_array())
        for (const auto& m : j["messages"]) s.messages.push_back(msg_from_json(m));
    return s;
}

void Session::derive_title(size_t max_len) {
    for (const auto& m : messages) {
        if (m.role != "user" || m.content.empty()) continue;
        std::string t = m.content;
        // First line only, collapse whitespace edges.
        size_t nl = t.find('\n');
        if (nl != std::string::npos) t.resize(nl);
        if (t.size() > max_len) { t.resize(max_len - 1); t += "\u2026"; }
        title = t;
        return;
    }
    if (title.empty()) title = "(empty)";
}

SessionStore::SessionStore(const std::string& dir)
    : dir_(dir.empty() ? default_dir() : dir) {}

bool SessionStore::ensure_dir() const { return mkdirs(dir_); }

std::string SessionStore::new_id() {
    static long long counter = 0;
    return std::to_string(now_ms()) + "-" + std::to_string(counter++);
}

std::string SessionStore::path_for(const std::string& id) const {
    return dir_ + "/" + id + ".json";
}
std::string SessionStore::index_path() const {
    return dir_ + "/index.json";
}
std::string SessionStore::workspace_path() const {
    return dir_ + "/workspace.json";
}

bool SessionStore::save(Session& s) const {
    if (!ensure_dir()) return false;
    if (s.id.empty()) s.id = new_id();
    if (s.created_ms == 0) s.created_ms = now_ms();
    s.updated_ms = now_ms();
    std::ofstream f(path_for(s.id), std::ios::trunc);
    if (!f) return false;
    // Replace invalid UTF-8 rather than throw (model output can be dirty).
    f << s.to_json().dump(2, ' ', false, json::error_handler_t::replace);
    cache_valid_ = false;
    ::remove(index_path().c_str());
    return static_cast<bool>(f);
}

bool SessionStore::load(const std::string& id, Session& out) const {
    std::ifstream f(path_for(id));
    if (!f) return false;
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return false;
    out = Session::from_json(j);
    if (out.id.empty()) out.id = id;
    return true;
}

bool SessionStore::remove(const std::string& id) const {
    cache_valid_ = false;
    ::remove(index_path().c_str());
    return ::remove(path_for(id).c_str()) == 0;
}

std::vector<SessionMeta> SessionStore::list() const {
    if (cache_valid_) return list_cache_;

    // Try the index file first — much faster than opening every session file.
    std::ifstream idx(index_path());
    if (idx.is_open()) {
        try {
            json j;
            idx >> j;
            std::vector<SessionMeta> out;
            for (const auto& e : j.value("sessions", json::array())) {
                SessionMeta m;
                m.id = e.value("id", "");
                m.title = e.value("title", "");
                m.model = e.value("model", "");
                m.updated_ms = static_cast<long long>(e.value("updated_ms", 0ll));
                m.message_count = e.value("message_count", 0);
                m.file_size = static_cast<size_t>(e.value("file_size", 0));
                out.push_back(m);
            }
            list_cache_ = out;
            cache_valid_ = true;
            return list_cache_;
        } catch (...) {
            // Corrupt index — fall through to directory scan.
        }
    }
    std::vector<SessionMeta> out;
    DIR* d = ::opendir(dir_.c_str());
    if (!d) return out;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "index.json" || name == "workspace.json") continue;
        if (name.size() < 6 || name.substr(name.size() - 5) != ".json")
            continue;
        std::string id = name.substr(0, name.size() - 5);
        Session s;
        if (!load(id, s)) continue;
        struct stat st;
        SessionMeta m;
        m.id = s.id;
        m.title = s.title;
        m.model = s.model;
        m.updated_ms = s.updated_ms;
        m.message_count = static_cast<int>(s.messages.size());
        if (::stat(path_for(id).c_str(), &st) == 0)
            m.file_size = static_cast<size_t>(st.st_size);
        out.push_back(m);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(),
              [](const SessionMeta& a, const SessionMeta& b) {
                  return a.updated_ms > b.updated_ms;
              });
    list_cache_ = out;
    cache_valid_ = true;
    // Write index for next startup.
    if (!out.empty()) rebuild_index();
    return list_cache_;
}

bool SessionStore::list_contains(const std::string& id) const {
    for (const auto& m : list())
        if (m.id == id) return true;
    return false;
}

void SessionStore::rebuild_index() const {
    auto entries = list();  // uses cache
    json j;
    j["version"] = 1;
    json arr = json::array();
    for (const auto& e : entries) {
        arr.push_back({
            {"id", e.id},
            {"title", e.title},
            {"model", e.model},
            {"updated_ms", e.updated_ms},
            {"message_count", e.message_count},
            {"file_size", e.file_size}
        });
    }
    j["sessions"] = arr;
    std::ofstream f(index_path(), std::ios::trunc);
    if (f) f << j.dump(2);
}

bool SessionStore::save_workspace(const WorkspaceState& ws) const {
    if (!ensure_dir()) return false;
    std::ofstream f(workspace_path(), std::ios::trunc);
    if (!f) return false;
    f << ws.to_json().dump(2);
    return static_cast<bool>(f);
}

WorkspaceState SessionStore::load_workspace() const {
    std::ifstream f(workspace_path());
    if (!f) return {};
    try {
        json j;
        f >> j;
        return WorkspaceState::from_json(j);
    } catch (...) {
        return {};
    }
}

// ---------------------------------------------------------------------------
// WorkspaceState serialization
// ---------------------------------------------------------------------------

json WorkspaceState::to_json() const {
    json arr = json::array();
    for (const auto& w : windows) {
        arr.push_back({{"session_id", w.session_id}, {"title", w.title}});
    }
    return {{"version", 1}, {"windows", arr}, {"active", active}};
}

WorkspaceState WorkspaceState::from_json(const json& j) {
    WorkspaceState ws;
    ws.active = j.value("active", 0);
    for (const auto& e : j.value("windows", json::array())) {
        WindowEntry we;
        we.session_id = e.value("session_id", "");
        we.title = e.value("title", "");
        ws.windows.push_back(we);
    }
    return ws;
}

} // namespace agent
