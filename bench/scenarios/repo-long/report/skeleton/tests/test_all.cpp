#include <cstdio>
#include <string>
#include <vector>
#include "arith.h"
#include "text.h"
#include "inventory.h"
#include "report.h"

int main() {
    if (format_line({"apple", 2, 50}) != "apple: 2 @ 50c") { std::printf("format_line failed\n"); return 1; }
    std::printf("ok\n");
    return 0;
}
