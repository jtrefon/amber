#ifndef AMBER_PLUGINS_WATCH_SETTINGS_H
#define AMBER_PLUGINS_WATCH_SETTINGS_H

#include <string>

namespace agent::plugins {

struct WatchSettings {
    bool observer = true;
    int max_running = 32;
    int interval_s = 20;
    int deadline_s = 1800;
    int retention = 50;
    int retention_days = 7;
};

// Parse the plugin-owned watch.conf. Missing file or malformed values keep the
// defaults; the file is a user preference, not a startup blocker.
WatchSettings load_watch_settings(const std::string& path);
bool apply_watch_setting(WatchSettings& settings, const std::string& key, const std::string& value);
bool save_watch_settings(const std::string& path, const WatchSettings& settings);

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_SETTINGS_H
