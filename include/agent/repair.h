#ifndef AGENT_REPAIR_H
#define AGENT_REPAIR_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "agent/request_failure.h" // RequestFailure
#include "agent/llm.h"             // ModelCapabilities, ModelInfo

namespace agent {

// What a repair asks the retry loop to do next.
enum class RetryRequest : unsigned char {
    None,         // no repair applied; the turn degrades as it did before
    WithoutTools, // the tool grammar was the problem, so retry without tools
    WithTools     // the tools are fine; the model or the key was not
};

// The agent-side operations a repair needs. Strategies see only this, so each
// one is testable with a fake instead of a whole Agent, and the ladder does not
// grow a dependency on the loop, the context, or the hooks.
class RepairHost {
public:
    virtual ~RepairHost() = default;
    // The model currently configured, so a repair can tell "the server offers
    // what we already use" from a real replacement.
    virtual std::string model() const = 0;
    virtual void report_error(const std::string& kind, const std::string& message) = 0;
    // One line for the host's status bar, and the [harness] note the model
    // reads: the request it asked for is changing shape, which is its business.
    virtual void status(const std::string& line) = 0;
    virtual void announce(const std::string& note) = 0;
    virtual std::vector<ModelInfo> server_models() = 0;
    virtual void adopt_model(const std::string& id, const ModelCapabilities& caps) = 0;
    // Ask the host for a replacement key for `stage`. Empty means the host
    // cannot or will not provide one; the host owns the prompt and the debug
    // trail around it.
    virtual std::string request_api_key(const char* stage) = 0;
    virtual void adopt_api_key(const std::string& key) = 0;
};

// One repair in the ladder: it handles one kind of request failure and says
// which retry (if any) the loop should run. A strategy publishes its own error:
// "the server rejected the model" is the failure's record, whether or not a
// replacement is available.
class RepairStrategy {
public:
    virtual ~RepairStrategy() = default;
    virtual RequestFailure handles() const noexcept = 0;
    // `tools_present` is the request's shape, which one repair needs: dropping
    // tools from a request that has none is not a repair.
    virtual RetryRequest apply(const std::string& error_text, const std::string& stage,
                               bool tools_present, RepairHost& host) const = 0;
};

// The ordered ladder. The first strategy that handles the failure and can act
// decides the retry; one that cannot act does not stop a later strategy of the
// same kind from trying.
class RepairLadder {
public:
    void add(std::unique_ptr<RepairStrategy> strategy);
    RetryRequest repair(const std::string& error_text, const std::string& stage, bool tools_present,
                        RepairHost& host) const;
    std::size_t size() const noexcept { return strategies_.size(); }

private:
    std::vector<std::unique_ptr<RepairStrategy>> strategies_;
};

// The repairs amber ships, in preference order: drop the tools when the
// template's grammar cannot be built, swap to a model the server knows, then ask
// the host for a key. A new repair is a new strategy plus one line in
// make_default_repair_ladder(), not an edit to the loop.
std::unique_ptr<RepairStrategy> make_template_parser_repair();
std::unique_ptr<RepairStrategy> make_server_model_repair();
std::unique_ptr<RepairStrategy> make_api_key_repair();
RepairLadder make_default_repair_ladder();

} // namespace agent

#endif // AGENT_REPAIR_H
