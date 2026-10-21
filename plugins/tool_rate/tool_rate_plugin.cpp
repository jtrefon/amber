#include "tool_rate_plugin.h"

#include "agent/rate_governor.h"
#include "agent/run_scope.h"
#include "agent/tool.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <unistd.h>

namespace agent::plugins {

namespace {

constexpr double kMaxWaitSeconds = 300.0;
constexpr int kMaxManualIntervalMs = 60000;
constexpr int kMaxManualConcurrency = 8;
constexpr long kMaxManualTokens = 1000000000L;
constexpr double kMaxManualTtlSeconds = 3600.0;

std::string status_text(const RateStatus& s) {
    std::ostringstream out;
    out << "provider pacing: interval=" << s.interval_ms << "ms pause=" << s.pause_ms
        << "ms concurrency=" << s.concurrency << " in_flight=" << s.in_flight
        << " consecutive_rate_limits=" << s.consecutive_rate_limits
        << " tokens_in_window=" << s.tokens_in_window;
    if (s.token_capacity > 0)
        out << " token_capacity=" << s.token_capacity;
    if (s.request_capacity > 0)
        out << " request_capacity=" << s.request_capacity;
    if (s.manual)
        out << " manual_remaining=" << s.manual_remaining_ms << "ms";
    return out.str();
}

ToolResult bad(const std::string& why) {
    return {false, "", why};
}

ToolResult apply_throttle_set(ProviderGovernor& governor, const json& a) {
    const long long interval = a.value("interval_ms", 0LL);
    const long long concurrency = a.value("concurrency", 0LL);
    const long long token_cap = a.value("token_cap", 0LL);
    const double ttl = a.value("ttl_seconds", 300.0);
    if (interval < 0 || interval > kMaxManualIntervalMs)
        return bad("interval_ms out of range");
    if (concurrency < 0 || concurrency > kMaxManualConcurrency)
        return bad("concurrency out of range");
    if (token_cap < 0 || token_cap > kMaxManualTokens)
        return bad("token_cap out of range");
    if (!std::isfinite(ttl) || ttl < 1 || ttl > kMaxManualTtlSeconds)
        return bad("ttl_seconds out of range");
    governor.set_manual(std::chrono::milliseconds(interval), static_cast<int>(concurrency),
                        static_cast<long>(token_cap),
                        std::chrono::milliseconds(std::lround(ttl * 1000.0)));
    return {true, "", ""};
}

class WaitTool final : public Tool {
public:
    std::string name() const noexcept override { return "wait"; }
    std::string description() const noexcept override {
        return "Pause this turn for a bounded number of seconds. Use it after a provider "
               "rate-limit "
               "notice, or before a retry when the provider asked for a longer window. The wait is "
               "cancellable.";
    }
    json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"seconds", {{"type", "number"}, {"minimum", 0}, {"maximum", kMaxWaitSeconds}}},
                  {"reason", {{"type", "string"}}}}},
                {"required", json::array({"seconds"})}};
    }
    bool is_read_only() const noexcept override { return true; }
    ToolResult execute(const json& a) const override {
        if (!a.contains("seconds") || !a["seconds"].is_number())
            return bad("missing 'seconds'");
        const double seconds = a["seconds"].get<double>();
        if (!std::isfinite(seconds) || seconds < 0 || seconds > kMaxWaitSeconds)
            return bad("seconds must be between 0 and " + std::to_string(kMaxWaitSeconds));
        const int total_ms = static_cast<int>(std::lround(seconds * 1000.0));
        for (int waited = 0; waited < total_ms; waited += 100) {
            if (run_cancelled(CancellationToken{}))
                return bad("cancelled");
            if (usleep(100 * 1000) != 0)
                return bad("wait was interrupted");
        }
        std::string text = "waited " + std::to_string(seconds) + "s";
        if (a.contains("reason") && a["reason"].is_string() &&
            !a["reason"].get<std::string>().empty())
            text += ": " + a["reason"].get<std::string>();
        return {true, text, "", {{"seconds", seconds}}};
    }
};

class ThrottleTool final : public Tool {
public:
    std::string name() const noexcept override { return "throttle"; }
    std::string description() const noexcept override {
        return "Inspect or cap the active provider's request pacing. Status reports the current "
               "interval, pause, concurrency and token/request budget. Set applies a bounded, "
               "expiring ceiling (0 = no cap); reset clears it. It affects every request to that "
               "provider, including subagents.";
    }
    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"action", {{"type", "string"}, {"enum", json::array({"status", "set", "reset"})}}},
              {"interval_ms",
               {{"type", "integer"}, {"minimum", 0}, {"maximum", kMaxManualIntervalMs}}},
              {"concurrency",
               {{"type", "integer"}, {"minimum", 0}, {"maximum", kMaxManualConcurrency}}},
              {"token_cap", {{"type", "integer"}, {"minimum", 0}, {"maximum", kMaxManualTokens}}},
              {"ttl_seconds",
               {{"type", "number"}, {"minimum", 1}, {"maximum", kMaxManualTtlSeconds}}}}},
            {"required", json::array({"action"})}};
    }
    bool is_read_only() const noexcept override { return true; }
    ToolResult execute(const json& a) const override {
        const RunScopeChain* scope = current_run_scope();
        if (!scope || !scope->leaf.config)
            return bad("no active provider in this run");
        auto& governor = RateGovernorRegistry::global().for_config(*scope->leaf.config);
        const std::string action = a.value("action", std::string("status"));
        if (action == "reset") {
            governor.clear_manual();
        } else if (action == "set") {
            ToolResult applied = apply_throttle_set(governor, a);
            if (!applied.ok)
                return applied;
        } else if (action != "status") {
            return bad("action must be status, set or reset");
        }
        return {true, status_text(governor.status()), ""};
    }
};

} // namespace

std::vector<std::unique_ptr<Capability>> make_rate_tool_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "rate",
        [](PluginServices&) {
            std::vector<std::unique_ptr<Tool>> tools;
            tools.push_back(std::make_unique<WaitTool>());
            tools.push_back(std::make_unique<ThrottleTool>());
            return tools;
        },
        ToolCapability::Meta{{"wait", {"waiting", ToolRole::Control}},
                             {"throttle", {"pacing", ToolRole::Control}}}));
    caps.push_back(
        make_tool_doc_capability("wait_doc", tool_doc_priority::kRate, "prompts/tools/wait.md"));
    caps.push_back(make_tool_doc_capability("throttle_doc", tool_doc_priority::kRate,
                                            "prompts/tools/throttle.md"));
    return caps;
}

} // namespace agent::plugins
