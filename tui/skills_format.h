#ifndef AMBER_TUI_SKILLS_FORMAT_H
#define AMBER_TUI_SKILLS_FORMAT_H

#include <string>

namespace tui {

// Compact byte size shared by the detail pane and the install sheet, so the two
// surfaces cannot describe the same payload differently.
inline std::string human_size(long long bytes) {
    if (bytes >= 1024LL * 1024LL)
        return std::to_string(bytes / (1024LL * 1024LL)) + " MB";
    if (bytes >= 1024)
        return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

} // namespace tui

#endif // AMBER_TUI_SKILLS_FORMAT_H
