#include "tui/skills_sheet_view.h"

#include "tui/dialog.h"
#include "tui/keys.h"

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
    const char* audit =
        sheet.evidence().audited ? "audited" : "not audited (no audit source for this registry)";
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

// Word-wrap one logical line into `width` columns.
void wrap_line(const std::string& line, int width, std::vector<std::string>& out) {
    if (line.empty()) {
        out.emplace_back();
        return;
    }
    std::string current;
    size_t i = 0;
    while (i < line.size()) {
        size_t end = line.find(' ', i);
        if (end == std::string::npos)
            end = line.size();
        const std::string word = line.substr(i, end - i);
        if (!current.empty() && static_cast<int>(current.size() + 1 + word.size()) > width) {
            out.push_back(current);
            current.clear();
        }
        if (!current.empty())
            current += ' ';
        if (static_cast<int>(word.size()) > width) {
            // A long token (a URL, a hash) is broken rather than overflowing.
            size_t at = 0;
            while (at < word.size()) {
                if (!current.empty()) {
                    out.push_back(current);
                    current.clear();
                }
                current = word.substr(at, static_cast<size_t>(width));
                at += static_cast<size_t>(width);
                if (at < word.size()) {
                    out.push_back(current);
                    current.clear();
                }
            }
        } else {
            current += word;
        }
        i = end + 1;
    }
    out.push_back(current);
}

std::vector<std::string> wrap_body(const std::string& body, int width) {
    std::vector<std::string> lines;
    std::string logical;
    for (char ch : body) {
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            wrap_line(logical, width, lines);
            logical.clear();
        } else {
            logical += ch;
        }
    }
    if (!logical.empty())
        wrap_line(logical, width, lines);
    return lines;
}

} // namespace

namespace {

void paint_preview(WINDOW* w, const std::vector<std::string>& lines, int top, int height,
                   int width) {
    werase(w);
    for (int i = 0; i < height; ++i) {
        const int index = top + i;
        if (index >= static_cast<int>(lines.size()))
            break;
        mvwaddnstr(w, i, 0, lines[static_cast<size_t>(index)].c_str(), width);
    }
    const std::string position =
        std::to_string(std::min(top + height, static_cast<int>(lines.size()))) + "/" +
        std::to_string(lines.size());
    mvwaddnstr(w, height - 1, std::max(0, width - static_cast<int>(position.size())),
               position.c_str(), static_cast<int>(position.size()));
    wnoutrefresh(w);
    update_panels();
    doupdate();
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

} // namespace

void run_body_preview(const std::string& title, const std::string& body) {
    Dialog dlg(std::max(12, LINES - 2), std::max(40, COLS - 4), "Preview: " + title);
    dlg.set_footer({{"j/k", "scroll"}, {"Space", "page"}, {"Esc", "back"}});
    WINDOW* w = dlg.content();
    const int width = std::max(10, dlg.cols() - 3);
    const int height = std::max(1, dlg.rows() - 2);
    const std::vector<std::string> lines = wrap_body(body, width);
    int top = 0;
    dlg.show();
    while (true) {
        paint_preview(w, lines, top, height, width);
        const int ch = wgetch(w);
        if (ch == ERR)
            continue;
        if (ch == 27 || ch == 'q')
            return;
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
