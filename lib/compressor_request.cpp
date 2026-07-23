// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/compressor.h"

namespace agent {

Message build_compression_request(const std::vector<Message>& history) {
    (void)history;

    Message req;
    req.role = "user";
    req.content = R"(Analyze the conversation above and produce a JSON response with the following exact structure. Do not include any text outside the JSON block.

{
  "classification": [
    {"turns": "0-0", "tag": "core", "summary": ""},
    {"turns": "1-3", "tag": "context", "summary": "brief summary of what happened in these turns"},
    {"turns": "4-5", "tag": "prune", "summary": ""}
  ],
  "memories": [
    {"content": "fact learned from the conversation", "tags": ["tag1", "tag2"], "action": "upsert"}
  ],
  "skills": [
    {"content": "procedure description", "tags": ["tag1"], "trigger_phrase": "keyword", "action": "upsert"}
  ]
}

Tag meanings:
  "core"    = keep verbatim — active task, recent turns, decisions, preferences
  "context" = archive with summary — useful context but not immediately needed
  "prune"   = drop entirely — stale tool output, superseded attempts, loops

Memory/skill actions:
  "upsert"   = add or update this item (merge with existing by content hash)
  "deprecate" = mark as stale — evidence should be decremented

Summaries max 200 tokens per entry. Use contiguous turn ranges. Prefer single-turn ranges when possible.)";

    return req;
}

} // namespace agent
