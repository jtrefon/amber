#ifndef AMBER_TUI_WINDOW_MANAGER_H
#define AMBER_TUI_WINDOW_MANAGER_H

#include <memory>
#include <string>
#include <vector>

namespace agent {
class Config;
class ToolRegistry;
} // namespace agent

namespace tui {
struct Window;

class WindowManager {
public:
    WindowManager(agent::Config& cfg, agent::ToolRegistry& reg);
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

private:
    agent::Config& cfg_;
    agent::ToolRegistry& reg_;
    std::vector<std::unique_ptr<Window>> windows_;
    size_t active_ = 0;
    size_t next_id_ = 0;
};

} // namespace tui

#endif
