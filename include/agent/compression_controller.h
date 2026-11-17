#ifndef AGENT_COMPRESSION_CONTROLLER_H
#define AGENT_COMPRESSION_CONTROLLER_H

#include <cstddef>
#include <memory>
#include <vector>

#include "agent/compressor.h"

namespace agent {

class Context;
class LLMClient;

// The compression lifecycle: the gate that decides a pass is due, the pure
// pipeline over an immutable snapshot, the statistics the pass reports, and the
// cooldown that keeps a failing classifier from re-firing on every following
// turn.
//
// The agent keeps what a pass cannot own: the run scope, the rebuild of the
// live stack, the memory/brief application, and the host notifications. One
// controller belongs to one agent worker, so its state needs no lock.
class CompressionController {
public:
    CompressionController(std::unique_ptr<CompressionStrategy> strategy,
                          std::unique_ptr<CompressionGate> gate);
    ~CompressionController();
    CompressionController(CompressionController&&) noexcept;
    CompressionController& operator=(CompressionController&&) noexcept;
    CompressionController(const CompressionController&) = delete;
    CompressionController& operator=(const CompressionController&) = delete;

    // The numbers the gate decided on, for the host's debug log and the typed
    // CompressionEvent. Zero when no gate is attached.
    struct GateDecision {
        double tokens = 0;
        double budget = 0;
        double threshold = 0;
    };

    // Automatic compression needs both halves. A strategy without a gate still
    // supports manual /compress; a gate without a strategy can never run.
    bool auto_enabled() const noexcept;

    // Ask the gate whether this turn is due. False when either half is absent
    // or the gate declines.
    bool should_compress(const Context& context, const Config& cfg) const;
    GateDecision last_decision() const;
    void set_threshold(double threshold);
    void set_min_turns(int turns);

    // One pass. `context` is only read: the pipeline returns the replacement
    // list, and the caller rebuilds its stack on success.
    struct Request {
        const Context& context;
        LLMClient& client;
        const Config& cfg;
        const CompressionRequestPrefix& prefix;
        CompressionObserver* observer = nullptr;
        // The turn the cooldown is recorded against: one tool-using turn is
        // many LLM calls, so the unit is the real user prompt.
        std::size_t turn = 0;
    };

    struct Outcome {
        bool ok = false;
        // True when the pipeline actually ran. A refusal before it (no
        // strategy, a conversation too short) is not a failed attempt and must
        // not be reported as one.
        bool attempted = false;
        std::vector<Message> messages; // the replacement list, valid when ok
        CompressionResponse response;
        CompressionResult result;
    };
    Outcome run(const Request& request);

    // The last completed run. Manual /compress records failures too, so both
    // legs of a fork describe the same compression state.
    const CompressionResult& last() const noexcept;
    void set_last(CompressionResult result);

private:
    std::unique_ptr<CompressionStrategy> strategy_;
    std::unique_ptr<CompressionGate> gate_;
    CompressionResult last_;
};

} // namespace agent

#endif // AGENT_COMPRESSION_CONTROLLER_H
