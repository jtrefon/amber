#ifndef AMBER_TUI_PATH_CONFINE_H
#define AMBER_TUI_PATH_CONFINE_H

#include <string>

namespace tui {

// Resolve a user/model-supplied path against the workspace root and verify
// it stays inside. On success returns true and fills `resolved` with the
// absolute normalized path. On failure returns false and fills `err` with a
// human-readable reason. This is the single path-confinement authority for
// all TUI slash commands and @-reference expansion — no TUI code should
// build paths via string concatenation with the workspace root.
bool confine_path(const std::string& input, std::string& resolved,
                  std::string& err);

} // namespace tui

#endif
