#ifndef AMBER_PLUGINS_WATCH_TOOLS_H
#define AMBER_PLUGINS_WATCH_TOOLS_H

#include "watch_service.h"

#include <memory>

namespace agent {
class Tool;
}

namespace agent::plugins {

std::unique_ptr<agent::Tool> make_watch_tool(WatchService& service);

std::string format_watch_record(const WatchRecord& record);
std::string format_watch_report(const WatchReport& report, bool full);

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_TOOLS_H
