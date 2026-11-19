#include <cstdio>
#include <string>
#include <vector>
#include "arith.h"
#include "text.h"
#include "inventory.h"
#include "report.h"

int main() {
    if (format_line({"pear", 3, 75}) != "pear: 3 @ 75c") return 1;
    if (format_line({"none", 0, 0}) != "none: 0 @ 0c") return 1;
    return 0;
}
