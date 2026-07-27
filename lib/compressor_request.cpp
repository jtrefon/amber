// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/compressor.h"

namespace agent {

Message build_compression_request() {
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
    {"name": "project-uses-make", "content": "The amber project uses GNU Make with a custom ./configure script that generates Makefile from Makefile.in.", "tags": ["build", "make"], "action": "upsert"},
    {"name": "test-command", "content": "Tests are run via 'make test' which builds and runs the run_tests binary. There are 150+ unit tests.", "tags": ["testing", "make"], "action": "upsert"}
  ],
  "skills": [
    {"name": "running-tests", "content": "When the user asks to run tests: execute 'make test' in the workspace root. Report pass/fail counts.", "tags": ["testing"], "trigger_phrase": "test", "action": "upsert"},
    {"name": "build-project", "content": "When the user asks to build: run 'make' in the workspace root. The build produces amber, amber-tui, and libagent archive files.", "tags": ["build"], "trigger_phrase": "build", "action": "upsert"}
  ]
}

Tag meanings:
  "core"    = keep verbatim — active task, recent turns, decisions, preferences
  "context" = archive with summary — useful context but not immediately needed
  "prune"   = drop entirely — stale tool output, superseded attempts, loops

Memory/skill actions:
  "upsert"   = add or update this item (merge with existing by unique name)
  "deprecate" = mark as stale — evidence should be decremented

Guidelines:
  - "name" should be short, unique, kebab-case, and descriptive — it's the stable identifier
  - Keep content under 200 tokens per entry
  - Use contiguous turn ranges for classification. Prefer single-turn ranges
  - Up to ~20 memories and ~10 skills; leave unused slots empty rather than fabricating entries)";

    return req;
}

} // namespace agent
