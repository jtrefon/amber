#include "shapes.h"
#include <cstdio>
int main() {
    int fails = 0;
    for (double r = 1.0; r <= 4.0; r += 0.5) {
        double a = area_circle(r);
        std::printf("circle %g => %f\n", r, a);
        if (a < 3.14 * r * r * 0.999 || a > 3.14 * r * r * 1.001) ++fails;
    }
    for (double s = 1.0; s <= 5.0; s += 1.0) {
        double a = area_square(s);
        std::printf("square %g => %f\n", s, a);
        if (a != s * s) ++fails;
    }
    return fails == 0 ? 0 : 1;
}
