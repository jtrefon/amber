#include "solution.h"
#include <cstdio>
int main() {
    int fails = 0;
    for (int n = 1; n <= 30; ++n) {
        std::string expect;
        if (n % 15 == 0) expect = "FizzBuzz";
        else if (n % 3 == 0) expect = "Fizz";
        else if (n % 5 == 0) expect = "Buzz";
        else expect = std::to_string(n);
        std::string got = fizzbuzz(n);
        std::printf("%d=%s\n", n, got.c_str());
        if (got != expect) ++fails;
    }
    return fails == 0 ? 0 : 1;
}
