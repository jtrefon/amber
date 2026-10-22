#ifndef AMBER_PLUGINS_WATCH_SERVICE_H
#define AMBER_PLUGINS_WATCH_SERVICE_H

#include "watch_observer.h"
#include "watch_runner.h"
#include "watch_settings.h"
#include "watch_store.h"
#include "watch_types.h"

#include "agent/event_bus.h"
#include "agent/process.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace agent::plugins {

class WatchWorker;

// Plugin-owned registry and worker launcher. One worker thread per watch;
// watch work is process/poll work, not a model loop. The observer queue is
// serialized separately so a home GPU sees one small call at a time.
class WatchService {
public:
    WatchService(std::shared_ptr<CommandRunner> runner, std::shared_ptr<ObserverModel> observer,
                 WatchStore store, std::string observer_prompt, EventBus* events,
                 WatchSettings settings = {});
    ~WatchService();

    WatchService(const WatchService&) = delete;
    WatchService& operator=(const WatchService&) = delete;

    std::string start(const WatchStart& options, std::string& error);
    std::vector<WatchRecord> list() const;
    std::optional<WatchRecord> get(const std::string& id) const;
    std::optional<WatchReport> report(const std::string& id) const;
    bool cancel(const std::string& id);
    void cancel_owner(const std::string& owner_id);
    // The owner window is gone; terminal reports must not be re-emitted to it.
    void abandon_owner(const std::string& owner_id);
    // The host accepted a report into a turn. Persisted so a restart does not
    // re-emit it.
    void ack(const std::string& watch_id);
    // Reports that are terminal but not delivered/abandoned, in creation order.
    std::vector<WatchReport> pending_reports() const;
    // Re-publish undelivered reports after the host's windows are restored.
    void reemit_pending();
    std::optional<WatchRecord> wait(const std::string& id, int timeout_s) const;
    WatchCounts counts() const;
    int running_count() const;

    void shutdown();

    // Worker-facing callbacks.
    void update(const WatchRecord& record);
    void finish(const WatchRecord& record, const WatchReport& report);
    WatchReport observe(const WatchRecord& record, const WatchReport& fallback,
                        const CancellationToken& cancel);
    std::string artifact(const std::string& watch_id, const std::string& name,
                         const std::string& content);

private:
    std::string make_id();
    static bool validate_start(const WatchStart& options, std::string& error);
    WatchRecord make_record(const WatchStart& options);
    void launch(const WatchRecord& record);
    void mark_restarted_records();
    void prune();
    void publish_report(const WatchRecord& record, const WatchReport& report);

    std::shared_ptr<CommandRunner> runner_;
    std::shared_ptr<ObserverModel> observer_;
    WatchSettings settings_;
    WatchStore store_;
    std::string observer_prompt_;
    EventBus* events_ = nullptr;

    mutable std::mutex mtx_;
    std::map<std::string, WatchRecord> records_;
    std::map<std::string, WatchReport> reports_;
    std::map<std::string, std::shared_ptr<WatchWorker>> workers_;
    std::map<std::string, std::thread> threads_;
    mutable std::mutex observer_mtx_; // one observer call at a time
    std::atomic<int> observers_{0};
    std::atomic<bool> shutting_down_{false};
    std::atomic<unsigned long> counter_{0};
};

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_SERVICE_H
