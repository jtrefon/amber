#include "window_manager.h"
#include "window.h"

#include <agent.h>
#include <agent/compressor.h>
#include <agent/session.h>
#include <agent/plugin_runtime.h>
#include <agent/experience.h>
#include <agent/skill_catalog.h>
#include <agent/workspace.h>

#include <utility>

namespace tui {

WindowManager::WindowManager(agent::Config& cfg, agent::ToolRegistry& reg,
                             agent::PluginRuntime* plugin_runtime)
    : cfg_(cfg), reg_(reg), plugin_runtime_(plugin_runtime) {}

Window& WindowManager::new_window(const std::string& title, const std::string& project_root) {
    auto w = std::make_unique<Window>();
    w->kind = Window::Kind::Chat;
    w->id = next_id_++;
    w->owner_id = agent::SessionStore::new_id();
    w->project_root = project_root;
    w->title = title;
    agent::ScopedProjectRoot project_guard(project_root);
    auto agent_cfg = cfg_;
    if (!project_root.empty()) {
        agent_cfg.load_workspace_defaults(project_root + "/amber.conf");
        agent_cfg.load(agent::Workspace::settings_path_for(project_root));
    }
    // The gate reads the LIVE config on every check (threshold_exceeded uses
    // agent_cfg.context_size), so it is safe to build from the current cfg:
    // a late /set model or an autodetect window reaches the gate through the
    // agent's cfg_ the next time Agent::resolve_window runs.
    auto comp_cfg = agent::load_compression_config(agent_cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor();
    auto exp_cfg = agent::load_experience_config(agent_cfg);
    // Skills and learned memories are per-project data shared across every
    // window on this project (see shared_skills/shared_store).
    auto mem_store = shared_store(exp_cfg);
    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem_store);
    // cfg_ is the shared template — its cancel_token flag is shared too.
    // Give this window's agent a fresh flag so cancelling one run can
    // never cancel a sibling's.
    agent_cfg.cancel_token = agent::CancellationToken{};
    agent::Agent a(std::move(agent_cfg), reg_, agent::AgentHooks{}, std::move(compressor),
                   std::move(gate), std::move(mem_store), std::move(retriever), {}, {},
                   shared_skills());
    w->agent = std::make_unique<agent::Agent>(std::move(a));
    // The watch plugin routes a background report back to the window that
    // started it. The owner id is persisted with the window and survives a
    // restart, unlike the in-process window index.
    if (!project_root.empty())
        w->agent->set_project_root(project_root);
    w->agent->set_owner_id(w->owner_id);
    w->agent->policy().init(agent::Workspace::policy_path_for(
        project_root.empty() ? agent::Workspace::project_root() : project_root));
    // Attach the harness bus so plugins observe this window's turns. Without a
    // runtime (tests, headless hosts) the agent simply publishes nothing.
    if (plugin_runtime_) {
        w->agent->set_events(plugin_runtime_->events());
        w->agent->set_prompt_registry(plugin_runtime_->prompts());
    }
    windows_.push_back(std::move(w));
    set_active(windows_.size() - 1);
    return *windows_.back();
}

Window& WindowManager::new_resource_window(const std::string& kind, const std::string& id,
                                           const std::string& title) {
    auto w = std::make_unique<Window>();
    w->kind = Window::Kind::Resource;
    w->id = next_id_++;
    w->title = title;
    w->read_only = true;
    w->resource_kind = kind;
    w->resource_id = id;
    windows_.push_back(std::move(w));
    set_active(windows_.size() - 1);
    return *windows_.back();
}

Window& WindowManager::new_background_resource_window(const std::string& kind,
                                                      const std::string& id,
                                                      const std::string& title) {
    auto w = std::make_unique<Window>();
    w->kind = Window::Kind::Resource;
    w->id = next_id_++;
    w->title = title;
    w->read_only = true;
    w->resource_kind = kind;
    w->resource_id = id;
    windows_.push_back(std::move(w));
    return *windows_.back();
}

bool WindowManager::close_resource_window(const std::string& kind, const std::string& id) {
    for (size_t i = 0; i < windows_.size(); ++i) {
        const Window& window = *windows_[i];
        if (window.kind != Window::Kind::Resource || window.resource_kind != kind ||
            window.resource_id != id)
            continue;
        if (windows_.size() == 1)
            return false; // never leave the manager without a window
        const bool focused = active_ == i;
        windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(i));
        if (i < active_)
            --active_;
        else if (focused)
            active_ = std::min(last_chat_, windows_.size() - 1);
        return true;
    }
    return false;
}

std::size_t WindowManager::chat_count() const noexcept {
    std::size_t chats = 0;
    for (const auto& window : windows_)
        if (window->kind == Window::Kind::Chat)
            ++chats;
    return chats;
}

std::size_t WindowManager::chat_position() const noexcept {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < windows_.size(); ++i) {
        if (windows_[i]->kind == Window::Kind::Chat)
            ++seen;
        if (i == active_)
            break;
    }
    return seen;
}

void WindowManager::set_active(size_t idx) noexcept {
    active_ = idx;
    if (idx >= windows_.size())
        return;
    windows_[idx]->ever_focused = true;
    if (windows_[idx]->kind == Window::Kind::Chat)
        last_chat_ = idx;
}

Window* WindowManager::by_owner_id(const std::string& owner_id) {
    if (owner_id.empty())
        return nullptr;
    for (auto& window : windows_)
        if (window->kind == Window::Kind::Chat && window->owner_id == owner_id)
            return window.get();
    return nullptr;
}

bool WindowManager::focus_resource(const std::string& kind, const std::string& id) {
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (windows_[i]->kind == Window::Kind::Resource && windows_[i]->resource_kind == kind &&
            windows_[i]->resource_id == id) {
            set_active(i);
            return true;
        }
    }
    return false;
}

Window& WindowManager::open_welcome_window() {
    auto w = std::make_unique<Window>();
    w->id = next_id_++;
    w->title = "amber";
    w->read_only = true;
    w->welcome_art = true;
    windows_.push_back(std::move(w));
    active_ = windows_.size() - 1;
    return *windows_.back();
}

Window& WindowManager::ensure_chat_window() {
    if (!win().read_only)
        return win();
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (!windows_[i]->read_only) {
            active_ = i;
            return win();
        }
    }
    return new_window("chat");
}

Window& WindowManager::win() {
    return *windows_[active_];
}
const Window& WindowManager::win() const {
    return *windows_[active_];
}

std::shared_ptr<agent::SkillCatalog> WindowManager::shared_skills() {
    const std::string root = agent::Workspace::project_root();
    std::scoped_lock lk(pool_mtx_);
    auto& p = catalogs_[root];
    if (!p) {
        auto exp = agent::load_experience_config(cfg_);
        p = std::make_shared<agent::SkillCatalog>(cfg_);
        p->discover(store_for(root, exp)->top_skills(exp.max_skills, ""));
    }
    return p;
}

std::shared_ptr<agent::MemoryStore>
WindowManager::shared_store(const agent::ExperienceConfig& exp_cfg) {
    std::scoped_lock lk(pool_mtx_);
    return store_for(agent::Workspace::project_root(), exp_cfg);
}

std::shared_ptr<agent::MemoryStore>
WindowManager::store_for(const std::string& root, const agent::ExperienceConfig& exp_cfg) {
    auto& p = stores_[root];
    if (!p)
        p = agent::make_memory_store(exp_cfg);
    return p;
}

Window* WindowManager::by_id(size_t id) {
    for (auto& w : windows_)
        if (w && w->id == id)
            return w.get();
    return nullptr;
}

} // namespace tui
