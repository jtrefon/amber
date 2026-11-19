#include <cstdio>
#include <string>
#include <vector>
#include "arith.h"
#include "text.h"
#include "inventory.h"
#include "report.h"

int main() {
    std::vector<Item> items = {{"x", 4, 10}, {"y", 1, 90}, {"z", 0, 5}};
    if (total_units(items) != 5) return 1;
    if (total_units({}) != 0) return 1;
    return 0;
}
