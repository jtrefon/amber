#ifndef AMBER_PLUGINS_WATCH_STORE_H
#define AMBER_PLUGINS_WATCH_STORE_H

#include "watch_types.h"

#include <string>
#include <vector>

namespace agent::plugins {

// Plugin-owned persistence for watch records, reports and artifacts. The store
// is deliberately small: JSON files under the project's state directory, no
// database, no core session coupling.
class WatchStore {
public:
    explicit WatchStore(std::string root);

    const std::string& root() const noexcept { return root_; }
    std::vector<WatchRecord> load_records() const;
    bool save_record(const WatchRecord& record) const;
    bool save_report(const WatchReport& report) const;
    bool load_report(const std::string& watch_id, WatchReport& out) const;
    bool remove(const std::string& watch_id) const;
    std::string artifact_dir(const std::string& watch_id) const;
    std::string write_artifact(const std::string& watch_id, const std::string& name,
                               const std::string& content, std::size_t cap = 0) const;

private:
    std::string records_dir() const;
    std::string reports_dir() const;
    std::string artifacts_dir() const;

    std::string root_;
};

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_STORE_H
