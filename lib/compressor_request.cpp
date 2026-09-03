
#include "agent/compressor.h"

namespace agent {

Message build_classify_request(bool update_previous) {
    Message req;
    req.role = "user";
    std::string body = R"JSON(Analyze the conversation above. You are compressing it so work can continue with the most recent exchanges preserved verbatim and everything older reduced to a clear, self-contained summary.

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
    if (update_previous) {
        // A previous compression is visible above as a system message with
        // an archive. Extend it: keep its existing entries, only classify
        // turns AFTER that message, and never tag the compressed-context
        // message itself. The new summary should build on the prior one.
        body += R"JSON(

The conversation above already contains a "Compressed conversation context"
system message. Update it: classify only the turns that came AFTER that
message, leave the existing archive entries alone, tag the compressed-context
message itself as "core", and write the summary to cover BOTH the prior
compressed state and the new turns since it.)JSON";
    }
    req.content = std::move(body);
    return req;
}

// Build a request to extract memories and skills from the classification result.
// Called as a second step after classification, sharing the same KV prefix.
Message build_extract_request() {
    Message req;
    req.role = "user";
    req.content = R"JSON(Review the classification above. For each COMPLETED INVESTIGATION that was pruned, extract:

MEMORIES (facts about the codebase, project, or user):
  {"name": "<kebab-case-id>", "content": "One-sentence summary of the finding.",
   "tags": ["area"], "action": "upsert"}

SKILLS (reusable procedures):
  {"name": "<kebab-case-id>", "content": "One-sentence description.",
   "trigger_phrase": "<word>", "action": "upsert"}

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
