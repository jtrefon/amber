#include "sorting.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int fixtures[][8] = {
    {5, 4, 3, 2, 1, 0, 0, 0},
    {1, 2, 3, 4, 5, 0, 0, 0},
    {9, 1, 9, 1, 9, 1, 0, 0},
    {42, -7, 0, 13, -1, 100, -100, 3},
};

static int cmp_arrays(const int* a, const int* b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

int main() {
    int fails = 0;
    for (size_t f = 0; f < 4; ++f) {
        int* data = fixtures[f];
        size_t n = 0;
        while (n < 8 && data[n] != 0) ++n;
        if (n == 0) n = 8;
        int expected[8];
        std::memcpy(expected, data, sizeof(expected));
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
                if (expected[j] < expected[i]) {
                    int t = expected[i];
                    expected[i] = expected[j];
                    expected[j] = t;
                }
        int bubble[8], merged[8];
        std::memcpy(bubble, data, sizeof(bubble));
        std::memcpy(merged, data, sizeof(merged));
        bubble_sort(bubble, n);
        merge_sort(merged, n);
        std::printf("fixture %zu: %s %s\n", f,
                    cmp_arrays(bubble, expected, n) ? "bubble-ok" : "bubble-bad",
                    cmp_arrays(merged, expected, n) ? "merge-ok" : "merge-bad");
        if (!cmp_arrays(bubble, expected, n)) ++fails;
        if (!cmp_arrays(merged, expected, n)) ++fails;
    }
    return fails == 0 ? 0 : 1;
}
