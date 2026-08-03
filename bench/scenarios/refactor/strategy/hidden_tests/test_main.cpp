#include "pricing.h"
#include <cstdio>
int main() {
    int fails = 0;
    for (double w = 0.5; w <= 5.0; w += 0.5) {
        double s = shipping_cost("standard", w);
        double e = shipping_cost("express", w);
        std::printf("w=%g standard=%g express=%g\n", w, s, e);
        if (s != 10 + 2 * w) ++fails;
        if (e != 20 + 4 * w) ++fails;
    }
    double u = shipping_cost("overnight", 1.0);
    std::printf("unknown=%g\n", u);
    if (u != -1) ++fails;
    return fails == 0 ? 0 : 1;
}
