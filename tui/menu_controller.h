#ifndef AMBER_TUI_MENU_CONTROLLER_H
#define AMBER_TUI_MENU_CONTROLLER_H

#include <chrono>
#include <functional>
#include <string>

#include "tui/hints.h"
#include "tui/menu_model.h"
#include "tui/menu_view.h"

namespace tui {

// L2: owns the menu tree, the selection state and the idle hint rotator. The
// host supplies the action runner and the repaint; this class never touches
// ncurses or the agent directly.
class MenuController {
public:
    using ActionRunner = std::function<bool(const std::string&, const std::string&)>;
    using Redraw = std::function<void()>;
    using Status = std::function<void(const std::string&)>;

    MenuController(ActionRunner run, Redraw redraw, Status status);

    void load_data();
    void note_input(std::chrono::steady_clock::time_point now);
    std::string update_hint(std::chrono::steady_clock::time_point now, bool eligible);

    const menu::Model& model() const noexcept { return model_; }
    const menu::State& state() const noexcept { return state_; }
    const std::string& hint() const noexcept { return hint_text_; }
    bool is_open() const noexcept { return state_.mode != menu::Mode::Closed; }
    bool loaded() const noexcept { return loaded_; }

    void toggle();
    void close();
    bool handle_key(int ch);
    bool handle_hit(const menu::Hit& hit);

private:
    bool activate();
    bool handle_navigation(int ch);
    bool handle_letter(int ch);

    ActionRunner run_;
    Redraw redraw_;
    Status status_;
    menu::Model model_;
    menu::State state_;
    bool loaded_ = false;
    HintRotator hints_;
    std::string hint_text_;
    std::chrono::steady_clock::time_point last_input_{};
    std::chrono::steady_clock::time_point eligible_since_{};
    bool hint_was_eligible_ = false;
};

} // namespace tui

#endif // AMBER_TUI_MENU_CONTROLLER_H
