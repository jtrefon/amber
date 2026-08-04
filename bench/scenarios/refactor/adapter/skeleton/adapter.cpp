#include "adapter.h"

LoggerAdapter::LoggerAdapter(LegacyLogger& legacy) : legacy_(legacy) {}

void LoggerAdapter::info(const std::string& msg) {
    (void)msg;
}

void LoggerAdapter::error(const std::string& msg) {
    (void)msg;
}
