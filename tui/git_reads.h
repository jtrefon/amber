#ifndef AMBER_TUI_GIT_READS_H
#define AMBER_TUI_GIT_READS_H

#include "tui/textutil.h"

#include <string>

// The git subprocess reads behind the prompt. They live outside the renderer
// so the commands themselves -- not only their parsing -- can be exercised
// against a real repository in the unit suite. Both return an empty/zero read
// when git cannot report on the tree.
namespace tui {
namespace git_reads {

text::GitStatus read_status(const std::string& root);
text::DiffStat read_diff_stat(const std::string& root);

} // namespace git_reads
} // namespace tui

#endif // AMBER_TUI_GIT_READS_H
