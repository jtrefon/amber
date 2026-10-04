#include "tui/toggle_value.h"

namespace tui {

bool valid_toggle(const std::string& v) {
    return v == "on" || v == "off" || v == "toggle";
}

bool parse_toggle(const std::string& v, bool current) {
    if (v == "toggle")
        return !current;
    return v == "on";
}

} // namespace tui
