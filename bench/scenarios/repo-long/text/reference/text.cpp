#include "text.h"
#include <cctype>

std::string upper_text(const std::string& text) { std::string out = text; for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return out; }

int word_count(const std::string& text) {
    int count = 0;
    bool in = false;
    for (char c : text) { const bool space = c == ' ' || c == '\n' || c == '\t'; if (!space && !in) ++count; in = !space; }
    return count;
}
