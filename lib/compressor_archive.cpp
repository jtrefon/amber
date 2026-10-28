#include "agent/compressor.h"

#include <algorithm>
#include <cctype>

namespace agent {
namespace {

// Same bound cap_archive() enforces: entry count, then the token estimate the
// archive costs. Kept in one place by sending both callers here.
bool over_cap(const json& archive) {
    return archive.size() > kMaxArchiveEntries || archive.dump().size() / 4 > kMaxArchiveTokens;
}

bool is_archive_message(const Message& m) {
    return m.role == "system" && m.content.rfind(kCompressedContextPrefix, 0) == 0;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string strip_fences(const std::string& s) {
    if (s.rfind("```", 0) != 0)
        return s;
    const std::size_t body = s.find('\n');
    if (body == std::string::npos)
        return {};
    const std::size_t close = s.rfind("```");
    if (close == std::string::npos || close <= body)
        return trim(s.substr(body + 1));
    return trim(s.substr(body + 1, close - body - 1));
}

} // namespace

json archive_fold_candidates(const json& archive) {
    if (!archive.is_array() || archive.size() <= kArchiveKeepVerbatim || !over_cap(archive))
        return json::array();
    json out = json::array();
    for (std::size_t i = 0; i + kArchiveKeepVerbatim < archive.size(); ++i)
        out.push_back(archive[i]);
    return out;
}

json apply_archive_fold(const json& archive, const std::string& folded, std::size_t keep) {
    if (!archive.is_array() || folded.empty() || archive.size() <= keep)
        return archive;
    json out = json::array();
    out.push_back({{"turns", "earlier"}, {"summary", folded}});
    for (std::size_t i = archive.size() - keep; i < archive.size(); ++i)
        out.push_back(archive[i]);
    // Monotonic: a "summary" that costs as much as the entries it replaced is
    // not a summary, and swapping loses the detail for nothing.
    if (out.dump().size() >= archive.dump().size())
        return archive;
    return out;
}

Message build_archive_fold_request(const json& entries) {
    Message request;
    request.role = "user";
    request.content =
        "Fold these archived session notes into one shorter entry. Keep every fact a later "
        "turn depends on — files, decisions, constraints, open questions — and drop the "
        "narration. Reply with the entry text only: no JSON, no fences.\n\n" +
        entries.dump(2);
    return request;
}

std::string parse_archive_fold_response(const std::string& reply) {
    return strip_fences(trim(reply));
}

bool fold_stale_archive(std::vector<Message>& history, LLMClient& client,
                        CompressionObserver* observer) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (!is_archive_message(*it))
            continue;
        json body = json::parse(it->content.substr(std::string(kCompressedContextPrefix).size()),
                                nullptr, false);
        if (!body.is_object() || !body.contains("archive") || !body["archive"].is_array())
            return false;
        const json candidates = archive_fold_candidates(body["archive"]);
        if (candidates.empty())
            return false;
        // One standalone request: the archive is the payload, so replaying the
        // live prefix here would cost the context-sized prefill this pipeline
        // exists to avoid.
        if (observer)
            observer->on_llm_request_sent();
        Message reply;
        try {
            Stats stats;
            reply = client.chat({build_archive_fold_request(candidates)}, {}, &stats);
            if (observer)
                observer->on_llm_usage(stats);
        } catch (const std::exception&) {
            return false; // non-fatal: cap_archive() still bounds the archive
        }
        const json folded = apply_archive_fold(
            body["archive"], parse_archive_fold_response(reply.content), kArchiveKeepVerbatim);
        if (folded.dump() == body["archive"].dump())
            return false;
        body["archive"] = folded;
        it->content = std::string(kCompressedContextPrefix) + "\n" + body.dump(2);
        return true;
    }
    return false;
}

} // namespace agent
