#pragma once
#include <cstdio>
class LegacyLogger {
public:
    void log(const char* line) { std::printf("legacy: %s\n", line); }
};
