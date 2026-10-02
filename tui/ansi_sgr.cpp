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

namespace {

// Bold/dim/italic/underline toggles (and reset). Returns false for a code
// that is not an attribute, so the caller can try the colour table.
bool apply_attribute(int code, md::RunStyle& cur, const md::RunStyle& base) {
    switch (code) {
    case 0:
        cur = base;
        return true;
    case 1:
        cur.bold = true;
        return true;
    case 2:
        cur.dim = true;
        return true;
    case 3:
        cur.italic = true;
        return true;
    case 4:
        cur.under = true;
        return true;
    case 22:
        cur.bold = cur.dim = false;
        return true;
    case 23:
        cur.italic = false;
        return true;
    case 24:
        cur.under = false;
        return true;
    default:
        return false;
    }
}

// The bright colours 90-97 are the normal 30-37 set with the same targets.
int colour_base(int code) {
    return (code >= 90 && code <= 97) ? code - 60 : code;
}

} // namespace

void apply(int code, md::RunStyle& cur, const md::RunStyle& base, const md::Style& st) {
    if (apply_attribute(code, cur, base))
        return;
    switch (colour_base(code)) {
    case 30:
        cur.pair = st.text_pair;
        break;
    case 31:
        cur.pair = P_GAUGE_CRIT;
        break;
    case 32:
        cur.pair = st.code_pair;
        break;
    case 33:
        cur.pair = P_MD_CODESTR;
        break;
    case 34:
        cur.pair = P_MD_CODECMT;
        break;
    case 35:
        cur.pair = P_MD_CODEKEY;
        break;
    case 36:
        cur.pair = st.quote_pair;
        break;
    case 37:
        cur.pair = st.text_pair;
        break;
    default:
        break;
    }
}

} // namespace tui::ansi_sgr
