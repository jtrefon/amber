#include <cstdio>
#include <string>
#include <vector>
#include "arith.h"
#include "text.h"
#include "inventory.h"
#include "report.h"

int main() {
    if (add(20, 22) != 42) return 1;
    if (add(-2, 2) != 0) return 1;
    if (add(0, 0) != 0) return 1;
    return 0;
}
