#include "tui/component.h"

namespace tui {

void FocusManager::add(Component* component) {
    if (component)
        components_.push_back(component);
}

void FocusManager::clear() noexcept {
    components_.clear();
    focused_ = -1;
}

Component* FocusManager::focused() const noexcept {
    if (focused_ < 0 || static_cast<std::size_t>(focused_) >= components_.size())
        return nullptr;
    Component* component = components_[static_cast<std::size_t>(focused_)];
    return component && component->focusable() ? component : nullptr;
}

bool FocusManager::focus(Component* component) {
    for (std::size_t i = 0; i < components_.size(); ++i) {
        if (components_[i] != component)
            continue;
        if (!component->focusable())
            return false;
        focused_ = static_cast<int>(i);
        return true;
    }
    return false;
}

Component* FocusManager::step(int direction) {
    if (components_.empty())
        return nullptr;
    const int count = static_cast<int>(components_.size());
    int at = focused_;
    for (int tried = 0; tried < count; ++tried) {
        at = (at + direction + count) % count;
        if (components_[static_cast<std::size_t>(at)]->focusable()) {
            focused_ = at;
            return components_[static_cast<std::size_t>(at)];
        }
    }
    return nullptr;
}

Component* FocusManager::next() {
    return step(1);
}
Component* FocusManager::prev() {
    return step(-1);
}

bool FocusManager::dispatch(const Event& event) {
    Component* component = focused();
    return component && component->handle(event);
}

void HitMap::add(const Rect& area, Component* component) {
    if (component)
        entries_.push_back(Entry{area, component});
}

Component* HitMap::at(int x, int y) const {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->area.contains(x, y))
            return it->component;
    return nullptr;
}

} // namespace tui
