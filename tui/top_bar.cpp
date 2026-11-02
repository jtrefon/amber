#include "tui/top_bar.h"

#include "tui/textutil.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace tui::top_bar {

namespace {

constexpr int kOverflowReserve = 6; // "‹+99›" with a margin

std::string close_glyph(bool utf8) {
    return utf8 ? "\u00d7" : "x";
}

std::string truncate_cols(const std::string& s, int max_cols, bool utf8) {
    if (max_cols <= 0)
        return {};
    if (text::display_cols(s) <= max_cols)
        return s;
    const std::string tail = utf8 ? "\u2026" : "...";
    const int tail_w = text::display_cols(tail);
    if (max_cols < tail_w)
        return {};
    const int budget = max_cols - tail_w;
    int cols = 0;
    std::size_t end = 0;
    while (end < s.size()) {
        const std::size_t adv = text::utf8_len(s, end);
        const std::string cp = s.substr(end, adv);
        const int w = text::display_cols(cp);
        if (cols + w > budget)
            break;
        cols += w;
        end += adv;
    }
    return s.substr(0, end) + tail;
}

std::string tab_label(const Tab& tab, int index, int title_cols, bool utf8) {
    const std::string title =
        truncate_cols(tab.title.empty() ? "chat" : tab.title, title_cols, utf8);
    const std::string marker = attention_glyph(tab.running, tab.attention, utf8);
    return tab_open(utf8) + " " + std::to_string(index) + " " + title + " " + marker + " " +
           close_glyph(utf8) + " " + tab_close(utf8);
}

Role tab_role(const Tab& tab) {
    if (tab.active)
        return Role::ActiveTab;
    if (tab.attention)
        return Role::AttentionTab;
    if (tab.running)
        return Role::RunningTab;
    return Role::InactiveTab;
}

int index_digits(int index) {
    return static_cast<int>(std::to_string(index).size());
}

struct Labels {
    std::vector<std::string> text;
    std::vector<int> width;
};

Labels build_labels(const std::vector<Tab>& tabs, int title_cols, bool utf8) {
    Labels labels;
    labels.text.reserve(tabs.size());
    labels.width.reserve(tabs.size());
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        labels.text.push_back(tab_label(tabs[i], static_cast<int>(i) + 1, title_cols, utf8));
        labels.width.push_back(text::display_cols(labels.text.back()));
    }
    return labels;
}

int total_width(const std::vector<int>& widths) {
    int total = 0;
    for (int w : widths)
        total += w;
    return total;
}

Labels fit_all_titles(const std::vector<Tab>& tabs, int width, int used, bool utf8) {
    int title_cols = kTabMaxTitle;
    Labels labels = build_labels(tabs, title_cols, utf8);
    while (used + total_width(labels.width) > width && title_cols > kTabMinTitle) {
        --title_cols;
        labels = build_labels(tabs, title_cols, utf8);
    }
    return labels;
}

std::size_t active_index(const std::vector<Tab>& tabs) {
    for (std::size_t i = 0; i < tabs.size(); ++i)
        if (tabs[i].active)
            return i;
    return 0;
}

void append_all(Layout& out, const std::vector<Tab>& tabs, const Labels& labels, int used) {
    int x = used;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        out.spans.push_back({labels.text[i], tab_role(tabs[i])});
        if (tabs[i].active) {
            out.active_start = x;
            out.active_width = labels.width[i];
        }
        x += labels.width[i];
    }
}

void expand_active(Labels& labels, const std::vector<Tab>& tabs, std::size_t active, int available,
                   bool utf8) {
    const int digits = index_digits(static_cast<int>(active) + 1);
    if (available < 9 + digits)
        return;
    const int title_cols = std::clamp(available - 9 - digits, kTabMinTitle, kTabMaxTitle);
    labels.text[active] = tab_label(tabs[active], static_cast<int>(active) + 1, title_cols, utf8);
    labels.width[active] = text::display_cols(labels.text[active]);
}

std::vector<std::size_t> choose_shown(const std::vector<Tab>& tabs, const Labels& labels,
                                      std::size_t active, int width, int used, int reserve) {
    std::vector<std::size_t> shown{active};
    int shown_w = labels.width[active];
    std::size_t lo = active;
    std::size_t hi = active;
    while (true) {
        bool grew = false;
        if (hi + 1 < tabs.size() && used + shown_w + labels.width[hi + 1] + reserve <= width) {
            ++hi;
            shown.push_back(hi);
            shown_w += labels.width[hi];
            grew = true;
        }
        if (lo > 0 && used + shown_w + labels.width[lo - 1] + reserve <= width) {
            --lo;
            shown.push_back(lo);
            shown_w += labels.width[lo];
            grew = true;
        }
        if (!grew)
            break;
    }
    std::sort(shown.begin(), shown.end());
    return shown;
}

void shrink_active(Labels& labels, const std::vector<Tab>& tabs, std::size_t active, int available,
                   bool utf8) {
    if (labels.width[active] <= available)
        return;
    const int title_cols = std::max(0, available - 9 - index_digits(static_cast<int>(active) + 1));
    labels.text[active] = tab_label(tabs[active], static_cast<int>(active) + 1, title_cols, utf8);
    labels.width[active] = text::display_cols(labels.text[active]);
}

bool active_fits(const Labels& labels, std::size_t active, int available) {
    return labels.width[active] <= available;
}

Layout fallback(const std::vector<Tab>& tabs, int width, Layout out, bool utf8) {
    out.spans.resize(1);
    out.active_start = -1;
    out.active_width = 0;
    out.overflow = static_cast<int>(tabs.size());
    if (width > kAppMarkWidth) {
        const std::string more = tab_overflow(static_cast<int>(tabs.size()), utf8);
        if (kAppMarkWidth + text::display_cols(more) <= width)
            out.spans.push_back({more, Role::Overflow});
    }
    return out;
}

Layout layout_overflow(const std::vector<Tab>& tabs, Labels labels, int width, int used, bool utf8,
                       Layout out) {
    const std::size_t active = active_index(tabs);
    const int reserve = tabs.size() > 1 ? kOverflowReserve : 0;
    expand_active(labels, tabs, active, width - used - reserve, utf8);
    const std::vector<std::size_t> shown = choose_shown(tabs, labels, active, width, used, reserve);
    const int hidden = static_cast<int>(tabs.size() - shown.size());
    const int shown_reserve = hidden > 0 ? reserve : 0;
    const int available = width - used - shown_reserve;
    shrink_active(labels, tabs, active, available, utf8);
    if (!active_fits(labels, active, available))
        return fallback(tabs, width, std::move(out), utf8);

    int x = used;
    for (std::size_t idx : shown) {
        out.spans.push_back({labels.text[idx], tab_role(tabs[idx])});
        if (idx == active) {
            out.active_start = x;
            out.active_width = labels.width[idx];
        }
        x += labels.width[idx];
    }
    if (hidden > 0)
        out.spans.push_back({tab_overflow(hidden, utf8), Role::Overflow});
    out.overflow = hidden;
    return out;
}

} // namespace

int tab_width(int title_cols, int index) {
    return 9 + index_digits(index) + std::max(0, title_cols);
}

std::string app_mark(bool utf8) {
    return utf8 ? "\u25c6" : "*";
}

std::string tab_open(bool utf8) {
    return utf8 ? "\u2590" : "[";
}

std::string tab_close(bool utf8) {
    return utf8 ? "\u258c" : "]";
}

std::string tab_rule(bool utf8) {
    return utf8 ? "\u2550" : "=";
}

std::string tab_overflow(int hidden, bool utf8) {
    const std::string n = "+" + std::to_string(hidden);
    return utf8 ? "\u2039" + n + "\u203a" : "[" + n + "]";
}

std::string attention_glyph(bool running, bool attention, bool utf8) {
    if (attention)
        return "!";
    if (running)
        return utf8 ? "\u25d0" : "*";
    return " ";
}

std::string clock_text(std::time_t now) {
    std::tm tm{};
    if (!localtime_r(&now, &tm))
        return "--:--";
    char buf[8];
    if (std::strftime(buf, sizeof(buf), "%H:%M", &tm) == 0)
        return "--:--";
    return buf;
}

bool pulse_on(std::time_t now) noexcept {
    return now % 2 == 0;
}

std::string pulse_glyph(bool on, bool utf8) {
    if (!utf8)
        return on ? "*" : ".";
    return on ? "\u25cf" : "\u00b7";
}

Layout layout(const std::vector<Tab>& tabs, int width, bool utf8) {
    Layout out;
    if (width <= 0)
        return out;
    out.spans.push_back({app_mark(utf8), Role::AppMark});
    int used = kAppMarkWidth;
    if (tabs.empty())
        return out;
    if (width > used + kAppGap) {
        out.spans.push_back({"  ", Role::Separator});
        used += kAppGap;
    }

    Labels labels = fit_all_titles(tabs, width, used, utf8);
    if (used + total_width(labels.width) <= width) {
        append_all(out, tabs, labels, used);
        return out;
    }
    return layout_overflow(tabs, std::move(labels), width, used, utf8, std::move(out));
}

} // namespace tui::top_bar
