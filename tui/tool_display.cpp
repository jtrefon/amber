
// RED state: stub implementations that deliberately fail the tool-display
// tests. Replaced by the real implementations in the green commit.

#include "tui/tool_display.h"

namespace tui {
namespace tool_display {

std::string describe_tool_call(const std::string& name,
                               const agent::json& args) {
    return name + " " + args.dump().substr(0, 60);
}

rich::Line close_tool_line(const rich::Line&, rich::Line summary) {
    return summary;
}

std::string elapsed_label(size_t) {
    return "0s";
}

std::string working_label(const std::string& frame, size_t) {
    return frame + " working 0s";
}

} // namespace tool_display
} // namespace tui
