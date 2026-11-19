#include <cstdio>
#include <string>
#include <vector>
#include "arith.h"
#include "text.h"
#include "inventory.h"
#include "report.h"

int main() {
    std::vector<Item> items = {{"a", 2, 100}, {"b", 3, 50}};
    if (total_units(items) != 5) { std::printf("total_units failed\n"); return 1; }
    if (total_cents(items) != 350) { std::printf("total_cents failed\n"); return 1; }
    std::printf("ok\n");
    return 0;
}
