#include "watch_settings.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

namespace agent::plugins {

namespace {

std::string trim(const std::string& s) {
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void apply_setting(WatchSettings& settings, const std::string& key, const std::string& value) {
    apply_watch_setting(settings, key, value);
}

} // namespace

namespace {

bool apply_watch_observer(WatchSettings& settings, const std::string& value) {
    const std::string v = lower(value);
    if (v == "off" || v == "false" || v == "0") {
        settings.observer = false;
        return true;
    }
    if (v == "on" || v == "auto" || v == "true" || v == "1") {
        settings.observer = true;
        return true;
    }
    return false;
}

bool apply_watch_int(WatchSettings& settings, const std::string& key, const std::string& value) {
    if (key != "max_running" && key != "interval_s" && key != "deadline_s" && key != "retention" &&
        key != "retention_days")
        return false;
    try {
        const int n = std::stoi(value);
        if (n <= 0)
            return false;
        if (key == "max_running")
            settings.max_running = n;
        else if (key == "interval_s")
            settings.interval_s = n;
        else if (key == "deadline_s")
            settings.deadline_s = n;
        else if (key == "retention")
            settings.retention = n;
        else
            settings.retention_days = n;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

bool apply_watch_setting(WatchSettings& settings, const std::string& key,
                         const std::string& value) {
    if (key == "observer")
        return apply_watch_observer(settings, value);
    return apply_watch_int(settings, key, value);
}

WatchSettings load_watch_settings(const std::string& path) {
    WatchSettings settings;
    std::ifstream in(path);
    if (!in)
        return settings;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        apply_setting(settings, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return settings;
}

bool save_watch_settings(const std::string& path, const WatchSettings& settings) {
    std::error_code ec;
    const std::filesystem::path file(path);
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    out << "observer=" << (settings.observer ? "on" : "off") << "\n";
    out << "max_running=" << settings.max_running << "\n";
    out << "interval_s=" << settings.interval_s << "\n";
    out << "deadline_s=" << settings.deadline_s << "\n";
    out << "retention=" << settings.retention << "\n";
    out << "retention_days=" << settings.retention_days << "\n";
    return static_cast<bool>(out);
}

} // namespace agent::plugins
