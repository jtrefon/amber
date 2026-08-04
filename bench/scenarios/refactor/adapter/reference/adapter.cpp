#include "adapter.h"

LoggerAdapter::LoggerAdapter(LegacyLogger& legacy) : legacy_(legacy) {}

void LoggerAdapter::info(const std::string& msg) {
    legacy_.log(("[INFO] " + msg).c_str());
}

void LoggerAdapter::error(const std::string& msg) {
    legacy_.log(("[ERROR] " + msg).c_str());
}
