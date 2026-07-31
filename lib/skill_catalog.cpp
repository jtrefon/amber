
#include "agent/skill_catalog.h"
#include "agent/workspace.h"

#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace fs = std::filesystem;

namespace agent {

namespace {

constexpr const char* kEnabled = "enabled";
constexpr const char* kForceEnabled = "force-enabled";
constexpr const char* kDisabled = "disabled";
constexpr const char* kBlocked = "blocked";
constexpr const char* kSuppressed = "suppressed";

bool valid_state(const std::string& s) {
    return s == "enable" || s == "disable" || s == "block";
}

std::string read_whole_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

SkillCatalog::SkillCatalog(const Config& cfg, const SkillScanPaths& paths,
                           std::string home)
    : cfg_(cfg), interop_enabled_(cfg.skills_interop) {
    if (cfg.skills_max_discovery > 0) max_discovery_ = cfg.skills_max_discovery;
    if (cfg.skills_body_budget_tokens > 0)
        body_budget_ = cfg.skills_body_budget_tokens;
    paths_ = paths.project.empty() ? default_scan_paths() : paths;
    if (home.empty()) home = global_config_dir();
    project_overrides_path_ = Workspace::local_dir() + "/skills.json";
    global_overrides_path_ = home + "/skills.json";
    load_overrides_file(global_overrides_path_);
    load_overrides_file(project_overrides_path_);
}

void SkillCatalog::discover(const std::vector<Skill>& learned) {
    entries_.clear();
    body_cache_.clear();
    activated_.clear();
    learned_ = learned;
    std::set<std::string> selected;
    std::vector<std::string> warnings;

    auto absorb_authored = [&](const std::string& root, SkillScope scope) {
        for (auto& f : scan_skill_dir(root, scope, &warnings)) {
            SkillEntry e;
            e.name = f.name;
            e.scope = scope;
            e.origin = SkillOrigin::Authored;
            e.path = f.path;
            e.meta = std::move(f.meta);
            apply_override_state(e.name, e.state);
            if (e.state == kDisabled || e.state == kBlocked) continue;
            bool shadowed = selected.count(e.name) > 0;
            if (shadowed) {
                if (e.state != kForceEnabled) continue;
                entries_.push_back(std::move(e));
                continue;
            }
            if (e.state == kForceEnabled) e.state = kEnabled;
            selected.insert(e.name);
            entries_.push_back(std::move(e));
        }
    };
    absorb_authored(paths_.project, SkillScope::Project);
    absorb_authored(paths_.global, SkillScope::Global);
    if (interop_enabled_) {
        absorb_authored(paths_.claude, SkillScope::Interop);
        absorb_authored(paths_.codex, SkillScope::Interop);
    }

    for (const auto& sk : learned) {
        SkillEntry e;
        e.name = sk.name;
        e.scope = SkillScope::Project;
        e.origin = SkillOrigin::Learned;
        e.meta.name = sk.name;
        e.meta.body = sk.content;
        if (selected.count(e.name)) {
            e.state = kSuppressed;
            entries_.push_back(std::move(e));
            continue;
        }
        apply_override_state(e.name, e.state);
        if (e.state == kDisabled || e.state == kBlocked) continue;
        selected.insert(e.name);
        entries_.push_back(std::move(e));
    }
}

const SkillEntry* SkillCatalog::lookup(const std::string& name) const {
    for (const auto& e : entries_) {
        if (e.name != name) continue;
        if (e.state == kEnabled || e.state == kForceEnabled) return &e;
    }
    return nullptr;
}

std::optional<std::string> SkillCatalog::read_body(const std::string& name) {
    const SkillEntry* e = lookup(name);
    if (!e) return std::nullopt;
    auto it = body_cache_.find(name);
    if (it != body_cache_.end()) return it->second;
    if (e->origin == SkillOrigin::Learned) {
        body_cache_[name] = e->meta.body;
        return e->meta.body;
    }
    std::string contents = read_whole_file(e->path + "/SKILL.md");
    if (contents.empty()) return std::nullopt;
    if (estimate_tokens(contents) > body_budget_) return std::nullopt;
    body_cache_[name] = contents;
    return contents;
}

std::optional<std::string> SkillCatalog::activate(const std::string& name) {
    auto body = read_body(name);
    if (!body) return std::nullopt;
    for (const auto& a : activated_)
        if (a.name == name) return body;
    activated_.push_back({name, *body});
    return body;
}

void SkillCatalog::refresh() { discover(learned_); }

bool SkillCatalog::apply_override(const std::string& name,
                                  const std::string& state,
                                  const std::string& note) {
    if (!valid_state(state)) return false;
    overrides_[name] = {state, note};
    return save_overrides();
}

std::vector<std::string> SkillCatalog::discovery_block() const {
    std::vector<std::string> out;
    for (const auto& e : entries_) {
        if (e.state != kEnabled && e.state != kForceEnabled) continue;
        if (static_cast<int>(out.size()) >= max_discovery_) break;
        std::string desc = e.meta.description.empty() ? "(no description)"
                                                      : e.meta.description;
        out.push_back(e.name + ": " + desc);
    }
    return out;
}

void SkillCatalog::load_overrides_file(const std::string& path) {
    std::string contents = read_whole_file(path);
    if (contents.empty()) return;
    json doc;
    try {
        doc = json::parse(contents);
    } catch (...) {
        return;
    }
    if (!doc.is_object()) return;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        const std::string& name = it.key();
        const json& obj = it.value();
        if (!obj.is_object()) continue;
        std::string state = obj.value("state", "");
        std::string note = obj.value("note", "");
        if (valid_state(state)) overrides_[name] = {state, note};
    }
}

bool SkillCatalog::save_overrides() const {
    json doc = json::object();
    for (const auto& kv : overrides_)
        doc[kv.first] = {{"state", kv.second.state},
                         {"note", kv.second.note}};
    std::error_code ec;
    fs::path p(project_overrides_path_);
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(project_overrides_path_);
    if (!f) return false;
    f << doc.dump(2) << "\n";
    return static_cast<bool>(f);
}

void SkillCatalog::apply_override_state(const std::string& name,
                                        std::string& state) const {
    auto it = overrides_.find(name);
    state = kEnabled;
    if (it == overrides_.end()) return;
    if (it->second.state == "enable")
        state = kForceEnabled;
    else if (it->second.state == "disable")
        state = kDisabled;
    else if (it->second.state == "block")
        state = kBlocked;
}

int SkillCatalog::estimate_tokens(const std::string& text) noexcept {
    return static_cast<int>(text.size() / 4);
}

} // namespace agent
