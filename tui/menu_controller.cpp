#include "tui/menu_controller.h"

#include "agent/data_path.h"
#include "tui/keys.h"

#include <algorithm>
#include <fstream>

namespace tui {

MenuController::MenuController(ActionRunner run, Redraw redraw, Status status)
    : run_(std::move(run)), redraw_(std::move(redraw)), status_(std::move(status)),
      last_input_(std::chrono::steady_clock::now()) {}

void MenuController::load_data() {
    const std::string exe = agent::exe_path();
    const char* exe_path = exe.empty() ? nullptr : exe.c_str();
    for (const auto& path : agent::bundled_data_candidates("menus.json", exe_path)) {
        if (model_.load(path)) {
            loaded_ = true;
            break;
        }
    }
    std::vector<std::string> hints = default_hints();
    for (const auto& path : agent::bundled_data_candidates("hints.json", exe_path)) {
        auto loaded = tui::load_hints(path);
        if (!loaded.empty()) {
            hints = std::move(loaded);
            break;
        }
    }
    hints_.set_hints(std::move(hints));
}

void MenuController::note_input(std::chrono::steady_clock::time_point now) {
    last_input_ = now;
    hints_.reset();
    hint_text_.clear();
    hint_was_eligible_ = false;
}

std::string MenuController::update_hint(std::chrono::steady_clock::time_point now, bool eligible) {
    if (!eligible) {
        hints_.reset();
        hint_text_.clear();
        hint_was_eligible_ = false;
        return hint_text_;
    }
    if (!hint_was_eligible_) {
        eligible_since_ = now;
        hint_was_eligible_ = true;
    }
    const auto start = std::max(last_input_, eligible_since_);
    const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
    hint_text_ = hints_.update(idle, true);
    return hint_text_;
}

void MenuController::toggle() {
    if (!loaded_) {
        if (status_)
            status_("menu unavailable: menus.json was not found");
        return;
    }
    if (is_open())
        menu::close(state_);
    else
        menu::open(state_, model_);
    if (redraw_)
        redraw_();
}

void MenuController::close() {
    if (!is_open())
        return;
    menu::close(state_);
    if (redraw_)
        redraw_();
}

bool MenuController::activate() {
    const menu::Selection selection = menu::activate(state_, model_);
    if (!selection.valid) {
        if (redraw_)
            redraw_();
        return true;
    }
    menu::close(state_);
    const bool ok = run_ ? run_(selection.action, selection.arg) : false;
    if (!ok && status_)
        status_("menu action unavailable: " + selection.action);
    if (redraw_)
        redraw_();
    return true;
}

bool MenuController::handle_hit(const menu::Hit& hit) {
    if (hit.kind == menu::HitKind::Inside)
        return true;
    if (hit.kind == menu::HitKind::Title) {
        state_.mode = menu::Mode::Bar;
        state_.menu = hit.menu;
        menu::open_items(state_, model_);
        if (redraw_)
            redraw_();
        return true;
    }
    state_.menu = hit.menu;
    if (hit.submenu) {
        state_.submenu = hit.item;
    } else {
        state_.mode = menu::Mode::Items;
        state_.item = hit.item;
    }
    return activate();
}

bool MenuController::handle_navigation(int ch) {
    if (ch == 27) {
        menu::back(state_);
    } else if (ch == keys::kLeft) {
        if (state_.mode == menu::Mode::Submenu)
            menu::back(state_);
        else
            menu::move_menu(state_, model_, -1);
    } else if (ch == keys::kRight) {
        if (state_.mode != menu::Mode::Submenu)
            menu::move_menu(state_, model_, 1);
    } else if (ch == keys::kDown) {
        menu::move_item(state_, model_, 1);
    } else if (ch == keys::kUp) {
        menu::move_item(state_, model_, -1);
    } else {
        return false;
    }
    if (redraw_)
        redraw_();
    return true;
}

bool MenuController::handle_letter(int ch) {
    if (ch < 32 || ch >= 127)
        return false;
    if (menu::select_letter(state_, model_, static_cast<char>(ch)) && redraw_)
        redraw_();
    return true;
}

bool MenuController::handle_key(int ch) {
    if (ch == keys::kMouse)
        return false;
    if (ch == keys::kF10) {
        toggle();
        return true;
    }
    if (!is_open())
        return false;
    if (ch == '\n' || ch == '\r' || ch == keys::kEnter)
        return activate();
    if (handle_navigation(ch))
        return true;
    if (handle_letter(ch))
        return true;
    return true;
}

} // namespace tui
