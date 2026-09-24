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

void apply(int code, md::RunStyle& cur, const md::RunStyle& base, const md::Style& st) {
    switch (code) {
    case 0:
        cur = base;
        break;
    case 1:
        cur.bold = true;
        break;
    case 2:
        cur.dim = true;
        break;
    case 3:
        cur.italic = true;
        break;
    case 4:
        cur.under = true;
        break;
    case 22:
        cur.bold = cur.dim = false;
        break;
    case 23:
        cur.italic = false;
        break;
    case 24:
        cur.under = false;
        break;
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
    case 90:
        cur.pair = st.text_pair;
        break;
    case 91:
        cur.pair = P_GAUGE_CRIT;
        break;
    case 92:
        cur.pair = st.code_pair;
        break;
    case 93:
        cur.pair = P_MD_CODESTR;
        break;
    case 94:
        cur.pair = P_MD_CODECMT;
        break;
    case 95:
        cur.pair = P_MD_CODEKEY;
        break;
    case 96:
        cur.pair = st.quote_pair;
        break;
    case 97:
        cur.pair = st.text_pair;
        break;
    default:
        break;
    }
}

} // namespace tui::ansi_sgr
