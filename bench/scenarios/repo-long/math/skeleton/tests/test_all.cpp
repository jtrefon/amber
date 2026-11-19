#include <cstdio>
#include <string>
#include <vector>
#include "arith.h"
#include "text.h"
#include "inventory.h"
#include "report.h"

int main() {
    if (add(2, 3) != 5) { std::printf("add(2,3) failed\n"); return 1; }
    if (multiply(2, 3) != 6) { std::printf("multiply failed\n"); return 1; }
    std::printf("ok\n");
    return 0;
}
