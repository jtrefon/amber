#include "watch_observer.h"

#include "agent/llm.h"
#include "agent/prompt.h"

#include <algorithm>
#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

namespace agent::plugins {

namespace {

using json = nlohmann::json;

constexpr std::size_t kRawCap = 2048;

std::string trim(const std::string& s) {
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string clip(std::string s, std::size_t cap) {
    if (s.size() > cap)
        s.resize(cap);
    return s;
}

std::string json_slice(const std::string& raw) {
    const std::size_t first = raw.find('{');
    const std::size_t last = raw.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first)
        return {};
    return raw.substr(first, last - first + 1);
}

std::vector<std::string> strings_of(const json& j, const char* key) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return out;
    for (const auto& item : *it)
        if (item.is_string())
            out.push_back(item.get<std::string>());
    return out;
}

std::string string_of(const json& j, const char* key, const std::string& fallback) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return fallback;
    const std::string value = it->get<std::string>();
    return value.empty() ? fallback : value;
}

} // namespace

std::string load_observer_prompt() {
    return load_optional_prompt("prompts/watch/observer.md");
}

HttpObserverModel::HttpObserverModel(const agent::Config* config) : config_(config) {}

std::string HttpObserverModel::complete(const std::string& system_prompt, const std::string& brief,
                                        const CancellationToken& cancel) {
    if (!config_)
        return {};
    agent::Config cfg = *config_;
    cfg.stream = false;
    cfg.temperature = 0.0;
    cfg.max_tokens = std::min<std::size_t>(cfg.max_tokens, 700);
    cfg.thinking = "off";
    cfg.reasoning_effort = "off";
    cfg.cancel_token = cancel;
    try {
        agent::HttpLLMClient client(cfg);
        std::vector<agent::Message> messages;
        agent::Message system;
        system.role = "system";
        system.content = system_prompt;
        messages.push_back(std::move(system));
        agent::Message user;
        user.role = "user";
        user.content = brief;
        messages.push_back(std::move(user));
        agent::Message reply = client.chat(messages, {});
        return reply.content;
    } catch (const std::exception&) {
        return {};
    }
}

WatchReport parse_observer_report(const std::string& raw, const WatchReport& fallback) {
    WatchReport report = fallback;
    const std::string body = json_slice(raw);
    if (body.empty()) {
        report.format_error = true;
        report.observed.push_back("observer output was not valid JSON: " +
                                  clip(trim(raw), kRawCap));
        return report;
    }
    try {
        const json j = json::parse(body);
        const std::string headline = string_of(j, "headline", "");
        if (!headline.empty())
            report.headline = headline;
        const auto observed = strings_of(j, "observed");
        // The observer refines; it does not erase the watch's deterministic
        // facts (failed check names, exit codes, links).
        for (const auto& line : observed)
            report.observed.push_back(line);
        report.likely_cause = string_of(j, "likely_cause", "");
        report.next_action = string_of(j, "next_action", "");
        report.confidence = string_of(j, "confidence", "low");
        report.journal_update = string_of(j, "journal_update", report.journal_update);
        if (j.contains("needs_attention") && j["needs_attention"].is_boolean())
            report.needs_attention = report.needs_attention || j["needs_attention"].get<bool>();
    } catch (const std::exception&) {
        report.format_error = true;
        report.observed.push_back("observer output was not valid JSON: " +
                                  clip(trim(raw), kRawCap));
    }
    return report;
}

} // namespace agent::plugins
