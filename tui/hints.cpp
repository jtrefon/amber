#include "tui/hints.h"

#include <algorithm>
#include <fstream>
#include <numeric>
#include <random>

#include <nlohmann/json.hpp>

namespace tui {

HintRotator::HintRotator(std::vector<std::string> hints, std::uint32_t seed) : seed_(seed) {
    set_hints(std::move(hints));
}

void HintRotator::set_hints(std::vector<std::string> hints) {
    hints_.clear();
    for (auto& hint : hints)
        if (!hint.empty())
            hints_.push_back(std::move(hint));
    bag_.clear();
    next_ = 0;
    current_.clear();
    next_at_ = std::chrono::milliseconds{0};
    finished_ = false;
    refill();
}

void HintRotator::refill() {
    bag_.resize(hints_.size());
    std::iota(bag_.begin(), bag_.end(), 0);
    std::mt19937 rng(seed_++);
    std::shuffle(bag_.begin(), bag_.end(), rng);
    next_ = 0;
    finished_ = false;
}

void HintRotator::pick() {
    if (hints_.empty())
        return;
    if (bag_.empty() || next_ >= bag_.size())
        refill();
    current_ = hints_[bag_[next_++]];
    if (next_ >= bag_.size())
        finished_ = true;
}

void HintRotator::reset() {
    current_.clear();
    next_at_ = std::chrono::milliseconds{0};
    finished_ = false;
}

std::string HintRotator::update(std::chrono::milliseconds idle, bool eligible) {
    if (!eligible) {
        reset();
        return {};
    }
    if (hints_.empty() || finished_ || idle < kDelay)
        return {};
    if (current_.empty()) {
        pick();
        next_at_ = idle + kEvery;
        return current_;
    }
    if (idle >= next_at_) {
        pick();
        next_at_ = idle + kEvery;
    }
    return current_;
}

std::vector<std::string> load_hints(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return {};
    try {
        nlohmann::json json;
        file >> json;
        if (!json.is_object() || !json.contains("hints") || !json["hints"].is_array())
            return {};
        std::vector<std::string> hints;
        for (const auto& entry : json["hints"])
            if (entry.is_string() && !entry.get<std::string>().empty())
                hints.push_back(entry.get<std::string>());
        return hints;
    } catch (const std::exception&) {
        return {};
    }
}

std::vector<std::string> default_hints() {
    return {
        "Hint: F10 opens the top menu.",
        "Hint: click the amber mark to open the menu.",
        "Hint: Alt+1\u20269 jumps between sessions.",
        "Hint: Ctrl+N starts a new session.",
        "Hint: type / to browse commands; Tab completes.",
        "Hint: ? opens help for the command you are typing.",
        "Hint: /help lists every command.",
        "Hint: Esc cancels the running turn.",
        "Hint: Alt+M gives drag-select back to the terminal.",
        "Hint: the mouse wheel scrolls the transcript.",
        "Hint: /session list shows saved sessions.",
        "Hint: /project list shows known projects.",
        "Hint: /project open <path> opens another project.",
        "Hint: /skills browse opens the skills explorer.",
        "Hint: /panel opens plugin panels.",
        "Hint: /usage shows token spend and cache reads.",
        "Hint: /get config paths shows where amber keeps its files.",
        "Hint: /window list shows the open windows.",
        "Hint: click a tab to switch; click \u00d7 to close it.",
        "Hint: [s1/3] shows the active session among open sessions.",
    };
}

} // namespace tui
