#include "lcs.h"
#include <cstdio>
int main() {
    struct Case { const char* a; const char* b; int expect; };
    Case cases[] = {
        {"", "abc", 0},
        {"abc", "", 0},
        {"abcde", "ace", 3},
        {"abc", "abc", 3},
        {"abcdef", "azbzcdef", 6},
        {"AGGTAB", "GXTXAYB", 4},
        {"banana", "anana", 5},
    };
    int fails = 0;
    for (auto& c : cases) {
        int got = lcs(c.a, c.b);
        std::printf("%s | %s => %d\n", c.a, c.b, got);
        if (got != c.expect) ++fails;
    }
    return fails == 0 ? 0 : 1;
}
