
#include "agent/compressor.h"

namespace agent {

Message build_classify_request(bool update_previous) {
    Message req;
    req.role = "user";
    std::string body = R"JSON(Analyze the conversation above. Your job is to reduce token waste while preserving everything the agent needs to continue working.

== STEP 1: Identify the current task ==
Read the last 3-5 turns. What is the agent actively working on right now?

== STEP 2: Classify ALL turn ranges ==
For every contiguous block of turns, assign one of:

  "core"    = KEEP VERBATIM — current active task, active files, recent decisions
  "prune"   = REMOVE ENTIRELY — completed investigations, competing branches,
              dead-end file reads, irrelevant grep results, loops
  "context" = ARCHIVE WITH ONE-LINE SUMMARY — build config, project structure,
              workflows discovered, settled decisions

Summaries must be at most 200 characters.

Respond with ONLY this JSON array — no text outside it, no markdown fences:
[
  {"turns": "0-5", "tag": "prune", "summary": ""},
  {"turns": "6-8", "tag": "core", "summary": ""},
  {"turns": "9-10", "tag": "context", "summary": "investigated config paths"}
])JSON";
    if (update_previous) {
        // A previous compression is visible above as a system message with
        // an archive. Extend it: keep its existing entries, only classify
        // turns AFTER that message, and never tag the compressed-context
        // message itself.
        body += R"JSON(

The conversation above already contains a "Compressed conversation context"
system message. Update it: classify only the turns that came AFTER that
message, leave the existing archive entries alone, and tag the
compressed-context message itself as "core".)JSON";
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
