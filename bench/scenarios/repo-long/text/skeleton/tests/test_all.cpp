#include <cstdio>
#include <string>
#include <vector>
#include "arith.h"
#include "text.h"
#include "inventory.h"
#include "report.h"

int main() {
    if (upper_text("abc") != "ABC") { std::printf("upper_text failed\n"); return 1; }
    if (word_count("a b  c") != 3) { std::printf("word_count failed\n"); return 1; }
    std::printf("ok\n");
    return 0;
}
