
#include "agent/compressor.h"

#include <cstddef>
#include <string>
#include <vector>

namespace agent {

// The classify prompt is data, not code: keeping it out of the function lets the
// request builder read as what it does.
constexpr const char* kClassifyPrompt =
    R"JSON(Analyze the conversation above. You are compressing it so work can continue with the most recent exchanges preserved verbatim and everything older reduced to a clear, self-contained summary.

== What to produce ==

1. WORK-STATE SUMMARY — a single, solid paragraph capturing:
   - What problem/task is being worked on right now.
   - What has been done so far (completed steps, key files touched, decisions made).
   - Important findings, constraints, or gotchas discovered.
   - What remains to do next.
   Write it densely but completely — after compression this summary plus the
   preserved recent tail is ALL the agent will have to continue from.

2. CLASSIFICATION of every older turn range into one of:
   "core"    = KEEP VERBATIM — ONLY the current active task's working set:
               the most recent user requests and their immediate tool activity.
   "prune"   = REMOVE ENTIRELY — completed investigations, dead ends, noise,
               irrelevant file reads/grep output, superseded attempts.
   "context" = ARCHIVE — anything useful but not active: build config,
               project structure, workflows, settled decisions, finished work.
               It belongs in the summary, not verbatim in the conversation.

Keep verbatim is the EXPENSIVE choice. Prefer "context" for anything older
than the current request, and "prune" for anything already reflected in the
summary. Summaries must be at most 200 characters.

Respond with ONLY this JSON object — no text outside it, no markdown fences:
{
  "summary": "<the work-state summary paragraph>",
  "classification": [
    {"turns": "0-5", "tag": "prune", "summary": ""},
    {"turns": "6-8", "tag": "core", "summary": ""},
    {"turns": "9-10", "tag": "context", "summary": "investigated config paths"}
  ]
})JSON";

// Appended when a previous compression is already in the conversation. The
// compressed-context block is merged into the leading system prompt on the wire
// (every dialect hoists system messages), so the instruction names its real
// position instead of pretending it sits inside the transcript.
constexpr const char* kClassifyUpdateSuffix = R"JSON(

The request begins with a "Compressed conversation context" block merged into
the system prompt. It is not one of the numbered transcript messages: leave its
archive entries alone, classify only the conversation turns after it, and write
the summary to cover BOTH the prior compressed state and the new turns.)JSON";

namespace {

// The index legend appended to the classify instruction. The classifier cannot
// count messages it only sees as text, so it is told their numbers explicitly.
// The manifest is part of the *instruction*: the cached conversation prefix is
// untouched (CC-04), and only the newest messages are listed individually —
// the older ones are grouped so a long session cannot inflate the instruction.
constexpr std::size_t kManifestTokens = 2000;
constexpr std::size_t kPreviewChars = 60;

std::string preview_of(const std::string& text) {
    std::string out;
    out.reserve(kPreviewChars);
    for (const char c : text) {
        if (out.size() >= kPreviewChars)
            break;
        out.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
    }
    return out;
}

std::string index_manifest(const std::vector<Message>& history) {
    std::string out = "\n\n== Transcript index ==\n"
                      "The messages above are numbered as the harness indexes them. Index 0 is the "
                      "system prompt; every tool result has its own index. `turns` values in your "
                      "answer use these numbers.\n";
    if (history.size() <= 1)
        return out;
    std::vector<std::string> lines;
    std::size_t budget = kManifestTokens;
    for (std::size_t i = history.size(); i-- > 1 && budget > 0;) {
        std::string line =
            std::to_string(i) + " " + history[i].role + " " + preview_of(history[i].content);
        const std::size_t cost = (line.size() / 4) + 1;
        if (cost > budget && !lines.empty())
            break;
        budget -= cost < budget ? cost : budget;
        lines.push_back(std::move(line));
    }
    if (!lines.empty()) {
        const std::size_t oldest = std::stoul(lines.back().substr(0, lines.back().find(' ')));
        if (oldest > 1)
            out += "1-" + std::to_string(oldest - 1) +
                   " (grouped: older turns, classify by their summary)\n";
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it)
        out += *it + "\n";
    return out;
}

} // namespace

Message build_classify_request(bool update_previous, const std::vector<Message>& history) {
    Message req;
    req.role = "user";
    std::string body = kClassifyPrompt;
    body += index_manifest(history);
    if (update_previous)
        body += kClassifyUpdateSuffix;
    req.content = std::move(body);
    return req;
}

// Build a request to extract memories and skills from the classification result.
// Called as a second step after classification, sharing the same KV prefix.
Message build_extract_request() {
    Message req;
    req.role = "user";
    req.content =
        R"JSON(Review the classification above. For each COMPLETED INVESTIGATION that was pruned, extract:

MEMORIES (facts about the codebase, project, or user):
  {"name": "<kebab-case-id>", "content": "One-sentence summary of the finding.",
   "tags": ["area"], "action": "upsert"}

SKILLS (reusable procedures):
  {"name": "<kebab-case-id>", "content": "One-sentence description.",
   "trigger_phrase": "<word>", "action": "upsert"}

RETIREMENTS (a stored memory or skill is wrong or obsolete):
  {"name": "<the stored item's name>", "action": "deprecate"}

The trigger_phrase is the word the user might say that should activate this skill.
If nothing to extract, omit the array.

Respond with ONLY this JSON — no text outside, no markdown fences:
{
  "memories": [
    {"name": "bug-fix-parser-null", "content": "Parser segfault on empty input fixed by null check", "tags": ["parser", "bug"], "action": "upsert"}
  ],
  "skills": [
    {"name": "run-tests", "content": "Run 'make test' in workspace root", "trigger_phrase": "test", "action": "upsert"}
  ]
})JSON";

    return req;
}

} // namespace agent
