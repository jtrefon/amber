#ifndef AGENT_CHECKPOINT_FORMAT_H
#define AGENT_CHECKPOINT_FORMAT_H

#include "agent/checkpoint.h"

#include <string>
#include <vector>

namespace agent {

// The short SHA a person reads and copies.
std::string checkpoint_short_id(const Checkpoint& checkpoint);

// "turn 3 (abc12345)" or "before turn 4 (def67890)".
std::string checkpoint_describe(const Checkpoint& checkpoint);

// Local "YYYY-MM-DD HH:MM", or "unknown" for a missing timestamp.
std::string checkpoint_when(long long timestamp_s);

// One line per checkpoint: turn/phase, short SHA, changed files, time, prompt.
// The prompt is flattened and clipped so a pasted conversation stays a row.
std::vector<std::string> checkpoint_rows(const std::vector<Checkpoint>& checkpoints);

// `checkpoint_rows` joined with newlines.
std::string checkpoint_history_text(const std::vector<Checkpoint>& checkpoints);

} // namespace agent

#endif // AGENT_CHECKPOINT_FORMAT_H
