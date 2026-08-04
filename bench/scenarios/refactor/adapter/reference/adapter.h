#pragma once
#include <string>
#include "legacy.h"
class LoggerAdapter {
public:
    explicit LoggerAdapter(LegacyLogger& legacy);
    void info(const std::string& msg);
    void error(const std::string& msg);

private:
    LegacyLogger& legacy_;
};
