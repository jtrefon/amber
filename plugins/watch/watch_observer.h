#ifndef AMBER_PLUGINS_WATCH_OBSERVER_H
#define AMBER_PLUGINS_WATCH_OBSERVER_H

#include "watch_types.h"

#include "agent/config.h"
#include "agent/process.h"

#include <string>

namespace agent::plugins {

// The one-shot observer port. It receives a system prompt and a compact brief,
// and returns the model's raw text. No tools, no conversation, no loop.
class ObserverModel {
public:
    virtual ~ObserverModel() = default;
    virtual std::string complete(const std::string& system_prompt, const std::string& brief,
                                 const CancellationToken& cancel) = 0;
};

class HttpObserverModel final : public ObserverModel {
public:
    // Non-owning: the host's live config outlives the plugin runtime. The copy
    // is taken per call, so a provider/model switch reaches the next observer.
    explicit HttpObserverModel(const agent::Config* config);

    std::string complete(const std::string& system_prompt, const std::string& brief,
                         const CancellationToken& cancel) override;

private:
    const agent::Config* config_ = nullptr;
};

// Parse the observer's JSON object. `fallback` supplies the deterministic state
// and headline; the observer may refine the explanation but never override the
// watch's observed state. Invalid output is preserved as format_error text.
WatchReport parse_observer_report(const std::string& raw, const WatchReport& fallback);

std::string load_observer_prompt();

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_OBSERVER_H
