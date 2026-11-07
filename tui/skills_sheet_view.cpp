#include "tui/skills_sheet_view.h"

#include "tui/keys.h"
#include "tui/markdown.h"
#include "tui/modal_host.h"
#include "tui/rich.h"
#include "tui/screen_view.h"
#include "tui/text_screen.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace tui {

namespace {

std::string mark(bool on) {
    return on ? "(o)" : "( )";
}

std::string risk_line(const agent::RiskSignal& signal) {
    return "! line " + std::to_string(signal.line) + "  " + signal.kind + "  " + signal.excerpt;
}

void add_evidence(std::vector<std::string>& lines, const InstallSheet& sheet) {
    lines.emplace_back("Evidence");
    lines.push_back("  " + std::string(agent::audit_verdict_name(sheet.evidence().audit)));
    int shown = 0;
    for (const auto& signal : sheet.evidence().risks) {
        if (shown == 3)
            break;
        lines.push_back("  " + risk_line(signal));
        ++shown;
    }
    if (shown == 0)
        lines.emplace_back("  no shell, network or path signals");
}

} // namespace

PaintedDialog paint_install_sheet(const InstallSheet& sheet, Size screen) {
    const auto& evidence = sheet.evidence();
    const bool project = sheet.scope() == InstallScope::Project;

    DialogView view;
    view.title = "Install skill";
    view.footer = "Enter: install   p: read   Tab: scope   Esc: cancel";
    view.wrap_lines = false;

    view.lines.push_back(evidence.name + "  \u00b7  " + evidence.source + "  \u00b7  " +
                         agent::trust_tier_name(evidence.tier));
    std::string facts = std::to_string(evidence.files) + " files  \u00b7  " +
                        human_bytes(evidence.bytes) + "  \u00b7  ~" +
                        std::to_string(evidence.tokens) + " tokens";
    if (!evidence.license.empty())
        facts += "  \u00b7  " + evidence.license;
    view.lines.push_back(facts);
    view.lines.emplace_back();

    view.lines.push_back("Scope    " + mark(project) + " project  .amber/skills");
    view.lines.push_back("         " + mark(!project) + " global   ~/.config/amber/skills");
    std::string name_line = "Name     " + evidence.name;
    if (sheet.collision())
        name_line += sheet.replace() ? "   [replace]" : "   [name exists]";
    view.lines.push_back(name_line);
    view.lines.push_back(std::string("After    ") + (sheet.enabled()
                                                         ? "(o) enable now  ( ) install disabled"
                                                         : "( ) enable now  (o) install disabled"));
    view.lines.emplace_back();

    add_evidence(view.lines, sheet);
    view.lines.emplace_back();
    view.lines.push_back(sheet.status());
    return paint_dialog(view, screen);
}

namespace {

void show_install_sheet(const InstallEvidence& evidence, const std::string& body,
                        const InstallSheetOptions& options, InstallDecision& out) {
    InstallSheet sheet(evidence, options);
    ModalHost host;
    host.custom([&sheet](Size screen) { return paint_install_sheet(sheet, screen); },
                [&](int ch) {
                    switch (sheet.key(ch)) {
                    case InstallSheetAction::Confirm:
                        out = sheet.decision();
                        return DialogKey::Accept;
                    case InstallSheetAction::Cancel:
                        return DialogKey::Cancel;
                    case InstallSheetAction::Preview:
                        run_body_preview(evidence.name, body);
                        return DialogKey::Redraw;
                    default:
                        return DialogKey::Redraw;
                    }
                });
}

} // namespace

InstallDecision run_install_sheet(const InstallEvidence& evidence, const std::string& body,
                                  const InstallSheetOptions& options) {
    InstallDecision decision;
    show_install_sheet(evidence, body, options, decision);
    return decision;
}

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

// The next line (in `step` direction) containing the query, wrapping at the
// ends. Returns `from` when nothing matches.
int find_line(const std::vector<rich::Line>& lines, int from, const std::string& query, int step) {
    if (query.empty() || lines.empty())
        return from;
    const int count = static_cast<int>(lines.size());
    for (int i = 1; i <= count; ++i) {
        const int index = ((from + step * i) % count + count) % count;
        if (ci_find(line_text(lines[static_cast<std::size_t>(index)]), query))
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

} // namespace

namespace {

struct PreviewState {
    TextScreenView view;
    Size screen{80, 24};
    int width = 0;
    int top = 0;
    std::string search;
    bool searching = false;

    int height() const { return std::max(1, screen.h - 4); }

    void clamp_top() {
        top =
            std::max(0, std::min(top, std::max(0, static_cast<int>(view.lines.size()) - height())));
    }

    void fill(const std::string& body, Size size) {
        screen = size;
        const int next = std::max(10, size.w - 4);
        if (next != width) {
            width = next;
            view.lines = rich::rewrap_all(md::render(body), width);
        }
        clamp_top();
        view.top = top;
        view.search = search;
        view.position =
            std::to_string(std::min(top + height(), static_cast<int>(view.lines.size()))) + "/" +
            std::to_string(view.lines.size());
    }

    bool search_key(int ch) {
        if (ch == '\n' || ch == '\r' || ch == keys::kEnter) {
            searching = false;
            top = find_line(view.lines, std::max(0, top - 1), search, 1);
            clamp_top();
        } else if (ch == 27) {
            searching = false;
            search.clear();
        } else if (ch == 127 || ch == 8 || ch == keys::kBackspace) {
            if (!search.empty())
                search.pop_back();
        } else if (ch >= 32 && ch < 127) {
            search.push_back(static_cast<char>(ch));
        }
        return false;
    }

    // True closes the pager; the tick of a search never does.
    bool key(int ch) {
        if (searching)
            return search_key(ch);
        if (ch == 27 || ch == 'q')
            return true;
        if (ch == '/') {
            searching = true;
            search.clear();
            return false;
        }
        if (ch == 'n' || ch == 'N') {
            top = find_line(view.lines, top, search, ch == 'n' ? 1 : -1);
            clamp_top();
            return false;
        }
        top = preview_top(ch, top, height(), static_cast<int>(view.lines.size()));
        return false;
    }
};

} // namespace

void run_body_preview(const std::string& title, const std::string& body) {
    PreviewState state;
    state.view.title = "Preview: " + title;
    state.view.footer = "j/k: scroll   Space: page   /: search   n/N: next/prev   Esc: back";

    ScreenHost host;
    host.run(
        [&](Size size) {
            state.fill(body, size);
            return paint_text_screen(state.view, size);
        },
        [&](int ch) { return state.key(ch); }, 250);
}

} // namespace tui
