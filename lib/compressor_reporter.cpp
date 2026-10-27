#include "agent/compressor.h"

#include "agent/agent.h"

#include <chrono>
#include <string>

namespace agent {

CompressionReporter::CompressionReporter(const AgentHooks& hooks, CompressionResult& result,
                                         std::function<void()> progress)
    : hooks_(hooks), r_(result), progress_(std::move(progress)) {}

void CompressionReporter::set_before(size_t msgs, size_t tokens) {
    before_msgs_ = msgs;
    r_.messages_before = msgs;
    r_.tokens_before = tokens;
}

void CompressionReporter::on_compress_start(size_t msgs, size_t) {
    log("compressing " + std::to_string(msgs) + " messages...");
    t0_ = std::chrono::steady_clock::now();
    pump();
}

void CompressionReporter::on_loop_collapse(size_t removed) {
    log("loop collapse removed " + std::to_string(removed) + " messages");
    pump();
}

void CompressionReporter::on_progress(size_t tokens, size_t msgs) {
    // Emitted as the working set shrinks (collapse/prune/apply). Keep it a
    // compact status line so the host's context gauge can tick downward
    // without spamming the scrollback.
    log("  ... " + std::to_string(msgs) + " msgs, ~" + std::to_string(tokens) + " tok remain");
    pump();
}

void CompressionReporter::on_llm_request_sent() {
    log("sending LLM request...");
    pump();
}

void CompressionReporter::on_llm_usage(const Stats& stats) {
    if (hooks_.on_usage)
        hooks_.on_usage(
            {UsageSource::Compression, stats.prompt_tokens, stats.completion_tokens, {}, {}});
}

void CompressionReporter::on_llm_reply_received(long sec) {
    log("LLM replied in " + std::to_string(sec) + "s");
    pump();
}

void CompressionReporter::on_parse_result(const CompressionResponse& cr) {
    size_t core = 0, prune = 0, ctx = 0;
    for (const auto& seg : cr.segments) {
        switch (seg.tag) {
        case Classification::core:
            ++core;
            break;
        case Classification::prune:
            ++prune;
            break;
        case Classification::context:
            ++ctx;
            break;
        }
    }
    std::string seg_info = std::to_string(cr.segments.size()) + " segments" + " (" +
                           std::to_string(core) + " core, " + std::to_string(prune) + " prune, " +
                           std::to_string(ctx) + " archive)";
    log("parsed " + seg_info + ", " + std::to_string(cr.memory_ops.size()) + " mem ops" + ", " +
        std::to_string(cr.skill_ops.size()) + " skill ops");

    // Show each memory/skill op that was extracted
    for (const auto& m : cr.memory_ops)
        log("  [mem] " + m.action + " \"" + m.name + "\"" +
            (m.content.empty() ? "" : ": " + m.content.substr(0, 100)));
    for (const auto& s : cr.skill_ops)
        log("  [skill] " + s.action + " \"" + s.name + "\"" +
            (s.content.empty() ? "" : ": " + s.content.substr(0, 100)));
    pump();
}

void CompressionReporter::on_apply_result(const CompressionResult&) {
    log("applied classification");
    pump();
}

void CompressionReporter::on_error(const std::string& msg) {
    log("error: " + msg);
    pump();
}

void CompressionReporter::on_compress_done(const CompressionResult& final) {
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    r_.messages_after = final.messages_after;
    r_.tokens_after = final.tokens_after;
    std::string detail;
    if (r_.messages_before > r_.messages_after)
        detail += std::to_string(r_.messages_before - r_.messages_after) + " msgs removed";
    if (r_.tokens_before > r_.tokens_after) {
        if (!detail.empty())
            detail += ", ";
        detail +=
            std::to_string(r_.tokens_before) + " → " + std::to_string(r_.tokens_after) + " tok";
    }
    if (detail.empty())
        detail = std::to_string(r_.tokens_after) + " tok";
    log("done in " + std::to_string(elapsed) + "s — " + detail);
}

void CompressionReporter::log(const std::string& msg) {
    if (hooks_.on_status)
        hooks_.on_status(msg);
}

void CompressionReporter::pump() {
    if (progress_)
        progress_();
}

} // namespace agent
