#include "tui/skills_sheet_view.h"

#include "tui/dialog.h"
#include "tui/keys.h"
#include "tui/markdown.h"
#include "tui/rich_paint.h"

#include <algorithm>
#include <string>
#include <vector>

#include <ncurses.h>

namespace tui {

namespace {

std::string mark(bool on) {
    return on ? "(o)" : "( )";
}

std::string risk_line(const agent::RiskSignal& signal) {
    return "! line " + std::to_string(signal.line) + "  " + signal.kind + "  " + signal.excerpt;
}

void paint_header(WINDOW* w, const InstallSheet& sheet, int aw) {
    const auto& evidence = sheet.evidence();
    const std::string identity = evidence.name + "  \u00b7  " + evidence.source + "  \u00b7  " +
                                 agent::trust_tier_name(evidence.tier);
    mvwaddnstr(w, 0, 0, identity.c_str(), std::max(0, aw - 1));

    std::string facts = std::to_string(evidence.files) + " files  \u00b7  " +
                        human_bytes(evidence.bytes) + "  \u00b7  ~" +
                        std::to_string(evidence.tokens) + " tokens";
    if (!evidence.license.empty())
        facts += "  \u00b7  " + evidence.license;
    mvwaddnstr(w, 1, 0, facts.c_str(), std::max(0, aw - 1));
    mvwhline(w, 2, 0, ACS_HLINE, std::max(0, aw));
}

void paint_settings(WINDOW* w, const InstallSheet& sheet, int aw) {
    const bool project = sheet.scope() == InstallScope::Project;
    mvwaddnstr(w, 3, 0, ("Scope    " + mark(project) + " project  .amber/skills").c_str(),
               std::max(0, aw - 1));
    mvwaddnstr(w, 4, 0, ("         " + mark(!project) + " global   ~/.config/amber/skills").c_str(),
               std::max(0, aw - 1));

    std::string name_line = "Name     " + sheet.evidence().name;
    if (sheet.collision())
        name_line += sheet.replace() ? "   [replace]" : "   [name exists]";
    mvwaddnstr(w, 5, 0, name_line.c_str(), std::max(0, aw - 1));

    const std::string enable = sheet.enabled() ? "(o) enable now  ( ) install disabled"
                                               : "( ) enable now  (o) install disabled";
    mvwaddnstr(w, 6, 0, ("After    " + enable).c_str(), std::max(0, aw - 1));
}

void paint_evidence(WINDOW* w, const InstallSheet& sheet, int aw, int ah) {
    int y = 8;
    mvwaddnstr(w, y++, 0, "Evidence", std::max(0, aw - 1));
    const char* audit = agent::audit_verdict_name(sheet.evidence().audit);
    mvwaddnstr(w, y++, 2, audit, std::max(0, aw - 3));

    int shown = 0;
    for (const auto& signal : sheet.evidence().risks) {
        if (shown == 3 || y >= ah - 2)
            break;
        mvwaddnstr(w, y++, 2, risk_line(signal).c_str(), std::max(0, aw - 3));
        ++shown;
    }
    if (shown == 0 && y < ah - 2)
        mvwaddnstr(w, y++, 2, "no shell, network or path signals", std::max(0, aw - 3));
    if (y < ah - 1)
        mvwaddnstr(w, ah - 1, 0, sheet.status().c_str(), std::max(0, aw - 1));
}

void paint_sheet(WINDOW* w, const InstallSheet& sheet) {
    const int aw = getmaxx(w);
    const int ah = getmaxy(w);
    werase(w);
    paint_header(w, sheet, aw);
    paint_settings(w, sheet, aw);
    paint_evidence(w, sheet, aw, ah);
    wnoutrefresh(w);
    update_panels();
    doupdate();
}

} // namespace

namespace {

std::string line_text(const rich::Line& line) {
    std::string text;
    for (const auto& run : line.runs)
        text += run.text;
    return text;
}

bool ci_find(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return false;
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    return it != haystack.end();
}

void paint_preview(WINDOW* w, const std::vector<rich::Line>& lines, int top, int height, int width,
                   const std::string& search) {
    werase(w);
    for (int i = 0; i < height; ++i) {
        const int index = top + i;
        if (index >= static_cast<int>(lines.size()))
            break;
        rich::paint_line(w, i, width, lines[static_cast<size_t>(index)]);
    }
    std::string position = std::to_string(std::min(top + height, static_cast<int>(lines.size()))) +
                           "/" + std::to_string(lines.size());
    if (!search.empty())
        position = "/" + search + "   " + position;
    mvwaddnstr(w, height - 1, std::max(0, width - static_cast<int>(position.size())),
               position.c_str(), static_cast<int>(position.size()));
    wnoutrefresh(w);
    update_panels();
    doupdate();
}

// The next line (in `step` direction) containing the query, wrapping at the
// ends. Returns `from` when nothing matches.
int find_line(const std::vector<rich::Line>& lines, int from, const std::string& query, int step) {
    if (query.empty() || lines.empty())
        return from;
    const int count = static_cast<int>(lines.size());
    for (int i = 1; i <= count; ++i) {
        const int index = ((from + step * i) % count + count) % count;
        if (ci_find(line_text(lines[static_cast<size_t>(index)]), query))
            return index;
    }
    return from;
}

int preview_top(int ch, int top, int height, int count) {
    const int last = std::max(0, count - height);
    switch (ch) {
    case 'j':
    case keys::kDown:
        return std::min(last, top + 1);
    case 'k':
    case keys::kUp:
        return std::max(0, top - 1);
    case ' ':
    case keys::kNPage:
        return std::min(last, top + height);
    case 'b':
    case keys::kPPage:
        return std::max(0, top - height);
    case 'g':
    case keys::kHome:
        return 0;
    case 'G':
    case keys::kEnd:
        return last;
    default:
        return top;
    }
}

// Read a search query on the footer row. Enter keeps it, Esc clears it.
std::string read_search(WINDOW* w, int row, int width) {
    std::string query;
    while (true) {
        const std::string shown = "/" + query;
        mvwhline(w, row, 0, ' ', std::max(0, width));
        mvwaddnstr(w, row, 0, shown.c_str(), std::max(0, width));
        wrefresh(w);
        const int ch = wgetch(w);
        if (ch == ERR)
            continue;
        if (ch == '\n' || ch == '\r' || ch == keys::kEnter)
            return query;
        if (ch == 27) {
            query.clear();
            return query;
        }
        if (ch == 127 || ch == 8 || ch == keys::kBackspace) {
            if (query.empty())
                return query;
            query.pop_back();
        } else if (ch >= 32 && ch < 127) {
            query.push_back(static_cast<char>(ch));
        }
    }
}

} // namespace

void run_body_preview(const std::string& title, const std::string& body) {
    Dialog dlg(std::max(12, LINES - 2), std::max(40, COLS - 4), "Preview: " + title);
    dlg.set_footer({{"j/k", "scroll"},
                    {"Space", "page"},
                    {"/", "search"},
                    {"n/N", "next/prev"},
                    {"Esc", "back"}});
    WINDOW* w = dlg.content();
    const int width = std::max(10, dlg.cols() - 3);
    const int height = std::max(1, dlg.rows() - 2);
    // The real renderer: what the model would read, not a summary of it.
    const std::vector<rich::Line> lines = rich::rewrap_all(md::render(body), width);
    int top = 0;
    std::string search;
    dlg.show();
    while (true) {
        paint_preview(w, lines, top, height, width, search);
        const int ch = wgetch(w);
        if (ch == ERR)
            continue;
        if (ch == 27 || ch == 'q')
            return;
        if (ch == '/') {
            search = read_search(w, height - 1, width);
            top = find_line(lines, top - 1, search, 1);
            continue;
        }
        if (ch == 'n' || ch == 'N') {
            top = find_line(lines, top, search, ch == 'n' ? 1 : -1);
            top = std::max(0, std::min(top, std::max(0, static_cast<int>(lines.size()) - height)));
            continue;
        }
        top = preview_top(ch, top, height, static_cast<int>(lines.size()));
    }
}

InstallDecision run_install_sheet(const InstallEvidence& evidence, const std::string& body,
                                  const InstallSheetOptions& options) {
    InstallSheet sheet(evidence, options);
    Dialog dlg(std::min(18, std::max(12, LINES - 2)), std::min(78, std::max(50, COLS - 4)),
               "Install skill");
    dlg.set_footer({{"Enter", "install"}, {"p", "read"}, {"Tab", "scope"}, {"Esc", "cancel"}});
    WINDOW* w = dlg.content();
    dlg.show();
    while (true) {
        paint_sheet(w, sheet);
        const int ch = wgetch(w);
        if (ch == ERR)
            continue;
        switch (sheet.key(ch)) {
        case InstallSheetAction::Confirm:
            return sheet.decision();
        case InstallSheetAction::Cancel:
            return InstallDecision{};
        case InstallSheetAction::Preview:
            run_body_preview(evidence.name, body);
            break;
        default:
            break;
        }
    }
}

} // namespace tui
