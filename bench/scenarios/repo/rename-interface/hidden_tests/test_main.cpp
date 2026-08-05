#include "lib.h"
#include "util.h"
#include <cstdio>
int main() {
    int a = compute_result(21);
    int b = helper(10);
    std::printf("a=%d b=%d\n", a, b);
    return (a == 42 && b == 21) ? 0 : 1;
}
