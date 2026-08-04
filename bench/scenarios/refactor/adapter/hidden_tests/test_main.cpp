#include "adapter.h"
#include <cstdio>
int main() {
    LegacyLogger legacy;
    LoggerAdapter adapter(legacy);
    int fails = 0;
    adapter.info("hello");
    adapter.error("boom");
    std::printf("done\n");
    // Output must be exactly:
    //   legacy: [INFO] hello
    //   legacy: [ERROR] boom
    //   done
    return fails == 0 ? 0 : 1;
}
