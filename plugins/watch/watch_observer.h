#ifndef AMBER_PLUGINS_WATCH_OBSERVER_H
#define AMBER_PLUGINS_WATCH_OBSERVER_H

#include "watch_types.h"

#include "agent/config.h"
#include "agent/extensions.h"
#include "agent/process.h"

#include <mutex>
#include <string>

namespace agent::plugins {

// The one-shot observer port. It receives a system prompt and a compact brief,
// and returns the model's raw text. No tools, no conversation, no loop.
class ObserverModel {
public:
    virtual ~ObserverModel() = default;
    virtual std::string complete(const std::string& system_prompt, const std::string& brief,
                                 const CancellationToken& cancel) = 0;
    // Token spend of this model's calls since the last drain; the service
    // attributes it to the watch's owning window.
    virtual agent::UsageTotals take_usage() { return {}; }
};

class HttpObserverModel final : public ObserverModel {
public:
    // Non-owning: the host's live config outlives the plugin runtime. The copy
    // is taken per call, so a provider/model switch reaches the next observer.
    explicit HttpObserverModel(const agent::Config* config);

    std::string complete(const std::string& system_prompt, const std::string& brief,
                         const CancellationToken& cancel) override;

    // Token usage the observer's calls have cost, drained by the host so it can
    // attribute watch spend to the owning window.
    agent::UsageTotals take_usage();

private:
    const agent::Config* config_ = nullptr;
    mutable std::mutex usage_mtx_;
    agent::UsageTotals usage_;
};

// Parse the observer's JSON object. `fallback` supplies the deterministic state
// and headline; the observer may refine the explanation but never override the
// watch's observed state. Invalid output is preserved as format_error text.
WatchReport parse_observer_report(const std::string& raw, const WatchReport& fallback);

std::string load_observer_prompt();

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_OBSERVER_H
