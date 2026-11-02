#ifndef AMBER_TUI_TOP_BAR_H
#define AMBER_TUI_TOP_BAR_H

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

// L1 (pure): the top rail's view model. The painter maps roles to colours and
// ncurses calls; nothing here includes a terminal header.
namespace tui::top_bar {

enum class Role {
    AppMark,
    ActiveTab,
    InactiveTab,
    RunningTab,
    DoneTab,
    AttentionTab,
    ErrorTab,
    Overflow,
    Separator,
};

// Why a tab wants the user's eyes. The host owns the transition; the rail only
// paints it. NeedsInput wins over Error, which wins over Done.
enum class Attention {
    None,
    Done,
    NeedsInput,
    Error,
};

struct Tab {
    std::size_t id = 0;
    std::string title;
    bool active = false;
    bool running = false;
    Attention attention = Attention::None;
};

struct Span {
    std::string text;
    Role role = Role::InactiveTab;
};

// One clickable tab in the painted rail. `start` is the column of the opening
// cap; `width` covers the whole capped label, so a click anywhere on the tab
// selects it.
struct TabHit {
    std::size_t id = 0;
    int start = 0;
    int width = 0;
};

struct Layout {
    std::vector<Span> spans; // left zone, in display order
    std::vector<TabHit> hits;
    int active_start = -1; // column of the active tab's opening cap
    int active_width = 0;  // display width of the active tab
    int overflow = 0;      // sessions not shown
};

inline constexpr int kAppMarkWidth = 1;
inline constexpr int kAppGap = 2;
inline constexpr int kTabMinTitle = 5;
inline constexpr int kTabMaxTitle = 18;

// Lay out the app mark and as many tabs as fit in `width` display columns. The
// active tab is always present; the rest are dropped into the overflow count.
Layout layout(const std::vector<Tab>& tabs, int width, bool utf8 = true);

// The left-zone width a tab consumes at a given title allowance. Exposed so
// the painter and the tests agree on the arithmetic.
int tab_width(int title_cols, int index);

// OSX-style clock: HH:MM, local time, no seconds.
std::string clock_text(std::time_t now);

// The health pulse: one-second beat, lit on the even second.
bool pulse_on(std::time_t now) noexcept;
std::string pulse_glyph(bool on, bool utf8 = true);

std::string app_mark(bool utf8 = true);
std::string tab_open(bool utf8 = true);
std::string tab_close(bool utf8 = true);
std::string tab_rule(bool utf8 = true);
std::string tab_overflow(int hidden, bool utf8 = true);
std::string attention_glyph(bool running, Attention attention, bool utf8 = true);

} // namespace tui::top_bar

#endif // AMBER_TUI_TOP_BAR_H
