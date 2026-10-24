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
    Window& new_window(const std::string& title, const std::string& project_root = "");
    Window& new_resource_window(const std::string& kind, const std::string& id,
                                const std::string& title);
    // Open a viewer for background work without moving focus.
    Window& new_background_resource_window(const std::string& kind, const std::string& id,
                                           const std::string& title);
    // Close a viewer; focus returns to the last chat window when it was active.
    bool close_resource_window(const std::string& kind, const std::string& id);
    bool focus_resource(const std::string& kind, const std::string& id);
    Window& open_welcome_window();
    Window& ensure_chat_window();
    Window& win();
    const Window& win() const;
    Window* by_id(size_t id);
    // The chat window that owns background work with this owner_id, or null.
    Window* by_owner_id(const std::string& owner_id);
    size_t active() const noexcept { return active_; }
    // Activating a window marks it viewed; resources use that to decide
    // whether a terminal state may close them.
    void set_active(size_t idx) noexcept;
    size_t next_id() const noexcept { return next_id_; }
    std::vector<std::unique_ptr<Window>>& all() noexcept { return windows_; }
    const std::vector<std::unique_ptr<Window>>& all() const noexcept { return windows_; }
    size_t count() const noexcept { return windows_.size(); }
    // Chat windows only: resource viewers are not sessions and never inflate
    // the [s...] count.
    size_t chat_count() const noexcept;
    // 1-based position of the active window among chat windows, so the [s...]
    // cell never reads index > count while a resource viewer is focused.
    size_t chat_position() const noexcept;
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
    size_t last_chat_ = 0;
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
