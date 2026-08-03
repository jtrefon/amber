#include "process.h"

static int extract_normalize(int v) {
    return v < 0 ? -v : v;
}

static int extract_score(int x, int y, int z) {
    int score = x * 2 + y * 3 + z * 5;
    if (score < 0) score = 0;
    if (score > 100) score = 100;
    return score;
}

static std::string extract_render(int score, int total) {
    return "[score=" + std::to_string(score) + " total=" +
           std::to_string(total) + "]";
}

std::string process(int a, int b, int c) {
    const int x = extract_normalize(a);
    const int y = extract_normalize(b);
    const int z = extract_normalize(c);
    return extract_render(extract_score(x, y, z), x + y + z);
}
