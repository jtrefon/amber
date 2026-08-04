#include "process.h"

std::string process(int a, int b, int c) {
    int x = a < 0 ? -a : a;
    int y = b < 0 ? -b : b;
    int z = c < 0 ? -c : c;
    int score = x * 2 + y * 3 + z * 5;
    if (score < 0) score = 0;
    if (score > 100) score = 100;
    std::string out = "[score=";
    out += std::to_string(score);
    out += " total=";
    out += std::to_string(x + y + z);
    out += "]";
    return out;
}
