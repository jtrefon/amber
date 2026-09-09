
#include "agent/session_brief.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "nlohmann/json.hpp"

namespace agent {

using json = nlohmann::json;

namespace {

json brief_to_json(const SessionBrief& b) {
    return {
        {"intent", b.intent},
        {"direction", b.direction},
        {"done", b.done},
        {"earlier", b.earlier},
        {"next", b.next},
        {"avoid", b.avoid},
    };
}

SessionBrief brief_from_json(const json& j) {
    SessionBrief b;
    b.intent = j.value("intent", "");
    b.direction = j.value("direction", "");
    b.earlier = j.value("earlier", "");
    b.next = j.value("next", "");
    if (j.contains("done") && j["done"].is_array())
        for (const auto& d : j["done"])
            if (d.is_string()) b.done.push_back(d.get<std::string>());
    if (j.contains("avoid") && j["avoid"].is_array())
        for (const auto& a : j["avoid"])
            if (a.is_string()) b.avoid.push_back(a.get<std::string>());
    return b;
}

} // namespace

bool SessionBriefStore::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    try {
        json j;
        f >> j;
        if (j.is_object()) brief_ = brief_from_json(j);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool SessionBriefStore::save(const std::string& path) const {
    namespace fs = std::filesystem;
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << brief_to_json(brief_).dump(2);
    f.flush();
    f.close();
    return true;
}

void SessionBriefStore::merge(const SessionBrief& fresh) {
    // Intent / Direction / Next: replace (latest is truth).
    if (!fresh.intent.empty()) brief_.intent = fresh.intent;
    if (!fresh.direction.empty()) brief_.direction = fresh.direction;
    if (!fresh.next.empty()) brief_.next = fresh.next;

    // Done: append new entries, cap at kBriefMaxDone, fold older into earlier.
    for (const auto& d : fresh.done) {
        if (d.empty()) continue;
        brief_.done.push_back(d);
    }
    while (brief_.done.size() > kBriefMaxDone) {
        if (brief_.earlier.empty())
            brief_.earlier = brief_.done.front();
        else
            brief_.earlier += "; " + brief_.done.front();
        brief_.done.erase(brief_.done.begin());
    }

    // Avoid: append-only, cap at kBriefMaxAvoid (drop oldest).
    for (const auto& a : fresh.avoid) {
        if (a.empty()) continue;
        brief_.avoid.push_back(a);
    }
    while (brief_.avoid.size() > kBriefMaxAvoid)
        brief_.avoid.erase(brief_.avoid.begin());
}

std::string SessionBriefStore::render() const {
    if (empty()) return {};
    std::ostringstream out;
    out << "[session-brief]\n";
    if (!brief_.intent.empty()) out << "intent: " << brief_.intent << "\n";
    if (!brief_.direction.empty())
        out << "direction: " << brief_.direction << "\n";
    if (!brief_.done.empty() || !brief_.earlier.empty()) {
        out << "done:\n";
        for (const auto& d : brief_.done) out << "  - " << d << "\n";
        if (!brief_.earlier.empty())
            out << "  earlier: " << brief_.earlier << "\n";
    }
    if (!brief_.next.empty()) out << "next: " << brief_.next << "\n";
    if (!brief_.avoid.empty()) {
        out << "avoid:\n";
        for (const auto& a : brief_.avoid) out << "  - " << a << "\n";
    }
    out << "[/session-brief]";

    std::string result = out.str();
    if (result.size() > kBriefMaxRenderBytes)
        result.resize(kBriefMaxRenderBytes);
    return result;
}

bool SessionBriefStore::empty() const noexcept {
    return brief_.intent.empty() && brief_.direction.empty() &&
           brief_.done.empty() && brief_.earlier.empty() &&
           brief_.next.empty() && brief_.avoid.empty();
}

void SessionBriefStore::clear() noexcept {
    brief_ = {};
}

} // namespace agent
