#include "text.h"
#include <cctype>

std::string upper_text(const std::string& text) { return text; }

int word_count(const std::string& text) {
    int count = 0;
    bool in = false;
    for (char c : text) { const bool space = c == ' ' || c == '\n' || c == '\t'; if (!space && !in) ++count; in = !space; }
    return count;
}
