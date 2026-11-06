#include "tui/skills_screen_view.h"

#include "tui/glyphs.h"
#include "tui/textutil.h"

#include <algorithm>
#include <array>
#include <string>

namespace tui {

namespace {

std::string compact_installs(int installs) {
    if (installs >= 1000000)
        return std::to_string(installs / 1000000) + "." + std::to_string((installs / 100000) % 10) +
               "M";
    if (installs >= 1000)
        return std::to_string(installs / 1000) + "K";
    return std::to_string(installs);
}

std::string tab_label(SkillTab tab, int index) {
    return std::to_string(index + 1) + " " + skill_tab_name(tab);
}

const char* sort_name(SkillSort sort) {
    switch (sort) {
    case SkillSort::Relevance:
        return "relevance";
    case SkillSort::Installs:
        return "installs";
    case SkillSort::Name:
        return "name";
    }
    return "?";
}

std::string row_text(const SkillRow& row, bool marked, int width) {
    std::string badges;
    if (marked)
        badges += "\u2022";
    if (row.installed)
        badges += "*";
    if (row.update)
        badges += "+";
    badges += audit_badge(row.audit);
    if (row.unmanaged)
        badges += "?";

    std::string name = row.ref.name;
    if (name.size() > 26) {
        name.resize(25);
        name += "\u2026";
    }

    const std::string tail = compact_installs(row.ref.installs);
    const int tail_x = std::max(28, width - static_cast<int>(tail.size()) - 1);

    std::string line = name;
    if (!badges.empty()) {
        if (static_cast<int>(line.size()) < 28)
            line.append(static_cast<std::size_t>(28 - static_cast<int>(line.size())), ' ');
        line += badges;
    }
    if (width > 40 && tail_x > 33) {
        const std::string source =
            row.state.empty() ? row.ref.source : row.ref.source + " \u00b7 " + row.state;
        if (static_cast<int>(line.size()) < 32)
            line.append(static_cast<std::size_t>(32 - static_cast<int>(line.size())), ' ');
        line += source.substr(0, static_cast<std::size_t>(tail_x - 33));
    }
    if (static_cast<int>(line.size()) < tail_x)
        line.append(static_cast<std::size_t>(tail_x - static_cast<int>(line.size())), ' ');
    line += tail;
    return line;
}

std::string empty_message(const SkillsBrowseCore& core, const std::string& query) {
    if (core.tab() == SkillTab::Installed)
        return "No installed skills. Press 1 and search.";
    if (core.tab() == SkillTab::Updates)
        return "No updates. Press 6, then u to check.";
    if (!skill_tab_available(core.tab()))
        return "This shelf has no source yet; Top shows the proven list.";
    if (!query.empty())
        return "No matches.";
    return "Type a query to search.";
}

void build_facets(const SkillsBrowseCore& core, SkillsScreenView& view) {
    const bool focused = core.pane() == SkillPane::Facets;
    view.facets.push_back({focused ? "Filters (j/k, Enter, f)" : "Filters", true, false});
    const std::vector<FacetGroup> groups = core.facet_groups();
    if (groups.empty()) {
        view.facets.push_back({"load a detail", false, false});
        view.facets.push_back({"to see filters", false, false});
        return;
    }
    for (FacetGroup group : groups) {
        view.facets.push_back({facet_group_name(group), true, false});
        const auto counts = core.facet_counts(group);
        for (std::size_t i = 0; i < counts.size(); ++i) {
            const std::string& value = counts[i].first;
            const bool selected = core.facet_selected(group, value);
            const bool cursor =
                focused && core.facet_cursor().group == group && core.facet_cursor().index == i;
            view.facets.push_back({std::string(selected ? "[x] " : "[ ] ") + value + " (" +
                                       std::to_string(counts[i].second) + ")",
                                   false, cursor});
        }
        view.facets.push_back({std::string(), false, false});
    }
}

void build_list(const SkillsBrowseCore& core, const std::string& query, int list_w,
                SkillsScreenView& view) {
    const auto rows = core.visible();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const SkillRow& row = rows[i];
        view.rows.push_back({row_text(row, core.marked(row.ref.id), list_w),
                             static_cast<int>(i) == core.selection()});
    }
    if (rows.empty())
        view.empty_message = empty_message(core, query);
}

void build_detail(const SkillsBrowseCore& core, const std::map<std::string, DetailView>& details,
                  SkillsScreenView& view) {
    const auto current = core.current();
    const auto detail = current ? details.find(current->ref.id) : details.end();
    if (current && detail != details.end()) {
        view.detail = &detail->second;
        view.detail_scroll = core.detail_scroll();
    } else {
        view.detail_placeholder = "Enter to load details";
    }
}

} // namespace

SkillsScreenView build_skills_screen(const SkillsBrowseCore& core, const SkillsScreenInput& input,
                                     const std::map<std::string, DetailView>& details,
                                     Size screen) {
    const SkillsLayout layout = skills_layout(screen.w, screen.h);

    SkillsScreenView view;
    view.tab = core.tab();
    view.show_facets = layout.show_facets;
    view.show_detail = layout.show_detail;
    view.search_field = input.query.empty() ? "type to search (2+ chars)" : input.query;
    view.search_state = source_state(core.answered(), core.cached(), core.fetched_at_ms(),
                                     core.error(), input.now_ms);
    view.sort_label = std::string("sort: ") + sort_name(core.sort());
    if (!input.budget.empty() &&
        (core.tab() == SkillTab::Installed || core.tab() == SkillTab::Updates))
        view.budget = input.budget;

    if (layout.show_facets)
        build_facets(core, view);
    build_list(core, input.query, std::max(10, layout.list_w), view);
    if (layout.show_detail)
        build_detail(core, details, view);

    view.footer = input.footer;
    if (!layout.show_detail) {
        const std::string hint = " \u00b7 " + std::to_string(screen.w) + "x" +
                                 std::to_string(screen.h) + " is list-only, 60x18 + detail";
        if (static_cast<int>(view.footer.size() + hint.size()) < screen.w)
            view.footer += hint;
    }
    return view;
}

namespace {

void paint_search(Surface& surface, const SkillsScreenView& view, int width) {
    const int state_x = std::max(0, width - static_cast<int>(view.search_state.size()) - 1);
    surface.put_text(0, 0, "/ " + view.search_field, Style{Role::Text});
    if (!view.sort_label.empty())
        surface.put_text(std::max(0, state_x - static_cast<int>(view.sort_label.size()) - 2), 0,
                         view.sort_label, Style{Role::Muted});
    surface.put_text(state_x, 0, view.search_state, Style{Role::Muted});
}

void paint_tabs(Surface& surface, const SkillsScreenView& view, int width) {
    const std::array<SkillTab, 7> tabs = {
        SkillTab::ForYou,      SkillTab::Trending,  SkillTab::Top,    SkillTab::Official,
        SkillTab::Collections, SkillTab::Installed, SkillTab::Updates};
    int x = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const SkillTab kind = tabs[i];
        const std::string label = tab_label(kind, static_cast<int>(i));
        const Style style = kind == view.tab             ? Style{Role::Selected}
                            : !skill_tab_available(kind) ? Style{Role::Muted}
                                                         : Style{Role::Text};
        if (kind == view.tab)
            surface.fill({x, 1, std::min(static_cast<int>(label.size()) + 1, width - x), 1},
                         Cell{" ", style});
        surface.put_text(x, 1, label, style);
        x += static_cast<int>(label.size()) + 2;
        if (x >= width)
            break;
    }
    if (!view.budget.empty())
        surface.put_text(std::max(0, width - static_cast<int>(view.budget.size()) - 1), 1,
                         view.budget, Style{Role::Muted});
}

void paint_facets(Surface& surface, const SkillsScreenView& view, int width, int top, int height) {
    for (int i = 0; i < static_cast<int>(view.facets.size()) && i < height; ++i) {
        const SkillsFacetLine& line = view.facets[static_cast<std::size_t>(i)];
        const Style style = line.header   ? Style{Role::Accent}
                            : line.cursor ? Style{Role::Selected}
                                          : Style{Role::Text};
        if (line.cursor)
            surface.fill({0, top + i, width, 1}, Cell{" ", style});
        surface.put_text(0, top + i, line.text, style);
    }
}

void paint_rows(Surface& surface, const SkillsScreenView& view, int x, int top, int width,
                int height) {
    if (view.rows.empty()) {
        surface.put_text(x, top, view.empty_message, Style{Role::Muted});
        return;
    }
    for (int i = 0; i < static_cast<int>(view.rows.size()) && i < height; ++i) {
        const SkillsListLine& line = view.rows[static_cast<std::size_t>(i)];
        const Style style{line.selected ? Role::Selected : Role::Text};
        if (line.selected)
            surface.fill({x, top + i, width, 1}, Cell{" ", style});
        surface.put_text(x, top + i, line.text, style);
    }
}

} // namespace

PaintedScreen paint_skills_screen(const SkillsScreenView& view, Size screen) {
    Surface surface(screen.w, screen.h);
    surface.clear(Cell{" ", Style{Role::Surface}});
    if (screen.w <= 0 || screen.h <= 0)
        return PaintedScreen{std::move(surface), -1, -1};

    const SkillsLayout layout = skills_layout(screen.w, screen.h);
    paint_search(surface, view, screen.w);
    paint_tabs(surface, view, screen.w);

    const Glyphs& glyphs = text::glyph::utf8() ? Glyphs::unicode() : Glyphs::ascii();
    surface.draw_hline(0, 2, screen.w, glyphs.hline, Style{Role::Frame});

    const int facet_w = layout.show_facets ? layout.facets_w : 0;
    const int list_x = facet_w;
    const int list_w = std::max(10, layout.list_w);
    const int detail_x = list_x + list_w + 2;
    const int detail_w = std::max(0, screen.w - detail_x - 1);
    const int top = 3;
    const int height = std::max(1, screen.h - 4);

    if (layout.show_facets)
        paint_facets(surface, view, std::max(0, facet_w - 1), top, height);
    paint_rows(surface, view, list_x, top, list_w, height);
    if (layout.show_detail) {
        if (view.detail)
            paint_detail_pane(surface, DetailRect{top, detail_x, height, detail_w}, *view.detail,
                              view.detail_scroll);
        else
            surface.put_text(detail_x, top, view.detail_placeholder, Style{Role::Muted});
    }
    if (!view.footer.empty())
        surface.put_text(0, top + height, view.footer, Style{Role::Muted});
    return PaintedScreen{std::move(surface), -1, -1};
}

} // namespace tui
