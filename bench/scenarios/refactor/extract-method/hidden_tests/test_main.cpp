#include "process.h"
#include <cstdio>
int main() {
    int pairs[][3] = {{1, 2, 3}, {-4, 5, -6}, {0, 0, 0}, {100, -100, 50}, {7, 8, 9}};
    int fails = 0;
    for (int i = 0; i < 5; ++i) {
        std::string out = process(pairs[i][0], pairs[i][1], pairs[i][2]);
        std::printf("%d %d %d => %s\n", pairs[i][0], pairs[i][1], pairs[i][2], out.c_str());
        int x = pairs[i][0] < 0 ? -pairs[i][0] : pairs[i][0];
        int y = pairs[i][1] < 0 ? -pairs[i][1] : pairs[i][1];
        int z = pairs[i][2] < 0 ? -pairs[i][2] : pairs[i][2];
        int score = x * 2 + y * 3 + z * 5;
        if (score < 0) score = 0;
        if (score > 100) score = 100;
        std::string expect = "[score=" + std::to_string(score) + " total=" +
                             std::to_string(x + y + z) + "]";
        if (out != expect) ++fails;
    }
    return fails == 0 ? 0 : 1;
}
