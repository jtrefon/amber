#ifndef AMBER_TUI_WINDOW_MANAGER_H
#define AMBER_TUI_WINDOW_MANAGER_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent {
struct Config;
struct ExperienceConfig;
class ToolRegistry;
class PluginRuntime;
class SkillCatalog;
class MemoryStore;
class MemoryRetriever;
} // namespace agent

namespace tui {
struct Window;

class WindowManager {
public:
    WindowManager(agent::Config& cfg, agent::ToolRegistry& reg,
                  agent::PluginRuntime* plugin_runtime = nullptr);
    Window& new_window(const std::string& title);
    Window& open_welcome_window();
    Window& ensure_chat_window();
    Window& win();
    const Window& win() const;
    Window* by_id(size_t id);
    size_t active() const noexcept { return active_; }
    void set_active(size_t idx) noexcept { active_ = idx; }
    size_t next_id() const noexcept { return next_id_; }
    std::vector<std::unique_ptr<Window>>& all() noexcept { return windows_; }
    const std::vector<std::unique_ptr<Window>>& all() const noexcept { return windows_; }
    size_t count() const noexcept { return windows_.size(); }
    bool valid_index(size_t idx) const noexcept { return idx < windows_.size(); }

    // The project's shared skill catalog / memory store. Skills and learned
    // memories are per-project data shared by every window on that project —
    // the pool is keyed by workspace root so a future per-window project
    // switch resolves a different pair with no further plumbing.
    std::shared_ptr<agent::SkillCatalog> shared_skills();
    std::shared_ptr<agent::MemoryStore> shared_store(const agent::ExperienceConfig& exp_cfg);

private:
    agent::Config& cfg_;
    agent::ToolRegistry& reg_;
    agent::PluginRuntime* plugin_runtime_ = nullptr;
    std::vector<std::unique_ptr<Window>> windows_;
    size_t active_ = 0;
    size_t next_id_ = 0;

    // Project-scoped shared state, keyed by workspace root. The pool is UI-
    // thread only today (window creation is a UI action); the mutex guards
    // against a future creation path off the main loop.
    std::mutex pool_mtx_;
    // Caller must hold pool_mtx_.
    std::shared_ptr<agent::MemoryStore> store_for(const std::string& root,
                                                  const agent::ExperienceConfig& exp_cfg);
    std::unordered_map<std::string, std::shared_ptr<agent::SkillCatalog>> catalogs_;
    std::unordered_map<std::string, std::shared_ptr<agent::MemoryStore>> stores_;
};

} // namespace tui

#endif
