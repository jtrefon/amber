#include <cstdio>
#include <string>
#include <vector>
#include "arith.h"
#include "text.h"
#include "inventory.h"
#include "report.h"

int main() {
    if (upper_text("amber") != "AMBER") return 1;
    if (upper_text("a1-b") != "A1-B") return 1;
    return 0;
}
