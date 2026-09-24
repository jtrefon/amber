#include "tui/ansi_sgr.h"

#include <cctype>

namespace tui::ansi_sgr {

std::vector<int> parse(const std::string& s, std::size_t i, std::size_t& next) {
    std::vector<int> nums;
    std::string num;
    std::size_t j = i + 2; // past ESC '['
    const std::size_t n = s.size();
    while (j < n) {
        const char ch = s[j];
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == ';') {
            if (ch == ';') {
                nums.push_back(num.empty() ? 0 : std::stoi(num));
                num.clear();
            } else {
                num += ch;
            }
            ++j;
        } else if (ch == 'm') {
            if (!num.empty())
                nums.push_back(std::stoi(num));
            ++j;
            break;
        } else {
            ++j;
            break;
        }
    }
    if (nums.empty())
        nums.push_back(0);
    next = j;
    return nums;
}

} // namespace tui::ansi_sgr
