#include "app.h"
#include <cstdio>
int main() {
    int v = run();
    std::printf("sum=%d\n", v);
    return v == 15 ? 0 : 1;
}
