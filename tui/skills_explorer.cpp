#include "tui/skills_explorer.h"

#include "agent/github_index.h"
#include "agent/skill_install.h"
#include "tui/dialog.h"
#include "tui/skills_lifecycle.h"
#include "tui/skills_service.h"
#include "tui/skills_sheet_view.h"

#include <algorithm>
#include <deque>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ncurses.h>

namespace tui {

namespace {

using Clock = std::chrono::steady_clock;

long long since(Clock::time_point then) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - then).count();
}

std::string failure_text(const agent::IndexFailure& error) {
    switch (error.kind) {
    case agent::IndexError::None:
        return {};
    case agent::IndexError::Disabled:
        return "index disabled (skills_index = none)";
    case agent::IndexError::Offline:
        return "offline · R retries";
    case agent::IndexError::Timeout:
        return "timed out · R retries";
    case agent::IndexError::Cancelled:
        return "cancelled";
    case agent::IndexError::RateLimited:
        return "rate limited" + (error.retry_after_s > 0
                                     ? " (retry in " + std::to_string(error.retry_after_s) + "s)"
                                     : " · R retries");
    case agent::IndexError::Http:
        return "registry returned HTTP " + std::to_string(error.status);
    case agent::IndexError::Malformed:
        return error.detail.empty() ? "malformed answer" : error.detail;
    case agent::IndexError::Unsupported:
        return "not supported";
    }
    return "error";
}

std::string compact_installs(int installs) {
    if (installs >= 1000000)
        return std::to_string(installs / 1000000) + "." + std::to_string((installs / 100000) % 10) +
               "M";
    if (installs >= 1000)
        return std::to_string(installs / 1000) + "K";
    return std::to_string(installs);
}

agent::SkillRef ref_from_id(const std::string& id) {
    agent::SkillRef ref;
    ref.id = id;
    ref.name = agent::skill_slug(id);
    const auto slash = id.rfind('/');
    if (slash != std::string::npos)
        ref.source = id.substr(0, slash);
    return ref;
}

const SkillRow* installed_row(const std::vector<SkillRow>& rows, const std::string& name) {
    for (const auto& row : rows) {
        if (row.ref.name == name)
            return &row;
    }
    return nullptr;
}

void draw_tabs(WINDOW* w, int row, int width, SkillTab tab) {
    const SkillTab tabs[] = {SkillTab::ForYou,   SkillTab::Trending,    SkillTab::Top,
                             SkillTab::Official, SkillTab::Collections, SkillTab::Installed,
                             SkillTab::Updates};
    int x = 0;
    for (std::size_t i = 0; i < std::size(tabs); ++i) {
        const SkillTab kind = tabs[i];
        const std::string label = std::to_string(i + 1) + " " + skill_tab_name(kind);
        if (kind == tab)
            wattron(w, A_REVERSE);
        else if (!skill_tab_available(kind))
            wattron(w, A_DIM);
        mvwaddnstr(w, row, x, label.c_str(), std::max(0, width - x));
        if (kind == tab)
            wattroff(w, A_REVERSE);
        else if (!skill_tab_available(kind))
            wattroff(w, A_DIM);
        x += static_cast<int>(label.size()) + 2;
        if (x >= width)
            break;
    }
}

struct RowFlags {
    bool selected = false;
    bool marked = false;
};

void draw_row(WINDOW* w, int y, int x, int width, const SkillRow& row, RowFlags flags) {
    std::string badges;
    if (flags.marked)
        badges += "\u2022";
    if (row.installed)
        badges += "*";
    if (row.update)
        badges += "+";
    if (row.audit == agent::AuditVerdict::Pass)
        badges += "\u2713";
    else if (row.audit == agent::AuditVerdict::Warn)
        badges += "!";
    else if (row.audit == agent::AuditVerdict::Fail)
        badges += "\u2717";
    if (row.unmanaged)
        badges += "?";

    std::string name = row.ref.name;
    if (name.size() > 26) {
        name.resize(25);
        name += "\u2026";
    }

    const std::string tail = compact_installs(row.ref.installs);
    const int tail_x = std::max(28, width - static_cast<int>(tail.size()) - 1);

    if (flags.selected)
        wattron(w, A_REVERSE);
    mvwhline(w, y, x, ' ', std::max(0, width));
    mvwaddnstr(w, y, x, name.c_str(), 27);
    if (!badges.empty())
        mvwaddnstr(w, y, x + 28, badges.c_str(), 4);
    if (width > 40) {
        const std::string source =
            row.state.empty() ? row.ref.source : row.ref.source + " · " + row.state;
        mvwaddnstr(w, y, x + 32, source.c_str(), std::max(0, tail_x - 33));
    }
    mvwaddnstr(w, y, x + tail_x, tail.c_str(), static_cast<int>(tail.size()));
    if (flags.selected)
        wattroff(w, A_REVERSE);
}

struct SkillDetailView {
    std::string id;
    std::string name;
    std::string source;
    std::string meta;
    std::string description;
    std::string category;
    std::string hash;
    int files = 0;
};

SkillDetailView view_of(const agent::SkillDetail& detail) {
    SkillDetailView view;
    view.id = detail.ref.id;
    view.name = detail.ref.name;
    view.source = detail.ref.source;
    view.meta = compact_installs(detail.ref.installs) + " installs";
    if (!detail.license.empty())
        view.meta += " · " + detail.license;
    view.description = detail.description;
    view.category = detail.category;
    view.hash = detail.hash;
    view.files = static_cast<int>(detail.assets.size());
    return view;
}

void draw_detail(WINDOW* w, int top, int left, int height, int width,
                 const std::optional<SkillDetailView>& detail) {
    if (width < 12 || height < 3)
        return;
    if (!detail) {
        mvwaddnstr(w, top, left, "Enter to load details", width);
        return;
    }
    int y = top;
    mvwaddnstr(w, y++, left, detail->name.c_str(), width);
    mvwaddnstr(w, y++, left, detail->source.c_str(), width);
    mvwaddnstr(w, y++, left, detail->meta.c_str(), width);
    if (!detail->description.empty()) {
        ++y;
        mvwaddnstr(w, y++, left, detail->description.c_str(), width);
    }
    if (!detail->category.empty()) {
        const std::string line = "Category  " + detail->category;
        mvwaddnstr(w, y++, left, line.c_str(), width);
    }
    mvwaddnstr(w, y++, left, ("Files     " + std::to_string(detail->files)).c_str(), width);
    if (!detail->hash.empty())
        mvwaddnstr(w, y++, left, ("Hash      " + detail->hash.substr(0, 12)).c_str(), width);
    if (y < top + height)
        mvwaddnstr(w, y, left, "i installs · Esc back", width);
}

InstallEvidence evidence_of(const agent::SkillDetail& detail, const std::string& name) {
    InstallEvidence evidence;
    evidence.name = name;
    evidence.source = detail.ref.source;
    evidence.category = detail.category;
    evidence.license = detail.license;
    evidence.files = static_cast<int>(detail.assets.size());
    for (const auto& asset : detail.assets)
        evidence.bytes += static_cast<long long>(asset.bytes);
    evidence.tokens = static_cast<int>(detail.body.size() / 4);
    evidence.risks = agent::scan_skill_body(detail.body);
    // The anonymous registry exposes no audits, so "community" is the honest
    // tier for a GitHub source and nothing is claimed as vetted.
    evidence.tier = agent::trust_tier(false, false, false,
                                      detail.ref.source.find('/') != std::string::npos, false);
    evidence.audit = detail.audit;
    evidence.audited = detail.audit == agent::AuditVerdict::Pass;
    return evidence;
}

std::string facet_line(const std::string& value, int count, bool selected) {
    return std::string(selected ? "[x] " : "[ ] ") + value + " (" + std::to_string(count) + ")";
}

struct FacetRect {
    int top = 0;
    int height = 0;
    int width = 0;
};

// One facet group, drawn from `y`, returning the next free line. Extracted so
// the column is a loop over groups, not five nested conditions.
int draw_facet_group(WINDOW* w, const SkillsBrowseCore& core, FacetGroup group, int y,
                     const FacetRect& rect, bool focused) {
    if (y >= rect.top + rect.height)
        return y;
    mvwaddnstr(w, y++, 0, facet_group_name(group), std::max(0, rect.width - 1));
    const auto counts = core.facet_counts(group);
    for (std::size_t i = 0; i < counts.size() && y < rect.top + rect.height; ++i) {
        const bool cursor =
            focused && core.facet_cursor().group == group && core.facet_cursor().index == i;
        const std::string line = facet_line(counts[i].first, counts[i].second,
                                            core.facet_selected(group, counts[i].first));
        if (cursor)
            wattron(w, A_REVERSE);
        mvwaddnstr(w, y++, 0, line.c_str(), std::max(0, rect.width - 1));
        if (cursor)
            wattroff(w, A_REVERSE);
    }
    return y + 1; // blank line between groups
}

// The facet column: one block per group, values ranked by count, the cursor
// reversed while the pane is focused. Selecting a value filters locally; the
// header says so when a live query is also narrowing the set.
void draw_facets(WINDOW* w, const SkillsBrowseCore& core, int top, int height, int width) {
    if (width < 8 || height < 3)
        return;
    const bool focused = core.pane() == SkillPane::Facets;
    mvwaddnstr(w, top, 0, focused ? "Filters (j/k, Enter, f)" : "Filters", std::max(0, width - 1));
    const std::vector<FacetGroup> groups = core.facet_groups();
    if (groups.empty()) {
        mvwaddnstr(w, top + 2, 0, "load a detail", std::max(0, width - 1));
        if (top + 3 < top + height)
            mvwaddnstr(w, top + 3, 0, "to see filters", std::max(0, width - 1));
        return;
    }
    const FacetRect rect{top, height, width};
    int y = top + 2;
    for (FacetGroup group : groups) {
        if (y >= top + height)
            return;
        y = draw_facet_group(w, core, group, y, rect, focused);
    }
}

struct ListRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

void paint_rows(WINDOW* w, const SkillsBrowseCore& core, const std::string& query,
                const ListRect& rect) {
    const auto rows_now = core.visible();
    for (int i = 0; i < rect.height; ++i) {
        const int index = core.scroll_offset() + i;
        if (index >= static_cast<int>(rows_now.size()))
            break;
        const SkillRow& row = rows_now[static_cast<size_t>(index)];
        draw_row(w, rect.y + i, rect.x, rect.width, row,
                 RowFlags{index == core.selection(), core.marked(row.ref.id)});
    }
    if (rows_now.empty()) {
        const char* message = "Type a query to search.";
        if (core.tab() == SkillTab::Installed)
            message = "No installed skills. Press 1 and search.";
        else if (core.tab() == SkillTab::Updates)
            message = "No updates. Press 6, then u to check.";
        else if (!skill_tab_available(core.tab()))
            message = "This shelf has no source yet; Top shows the proven list.";
        else if (!query.empty())
            message = "No matches.";
        mvwaddnstr(w, rect.y, rect.x, message, std::max(0, rect.width - 1));
    }
}

void paint_detail_pane(WINDOW* w, const SkillsBrowseCore& core,
                       const std::map<std::string, SkillDetailView>& details,
                       const ListRect& rect) {
    if (rect.width < 12)
        return;
    std::optional<SkillDetailView> view;
    if (const auto current = core.current()) {
        const auto it = details.find(current->ref.id);
        if (it != details.end())
            view = it->second;
    }
    draw_detail(w, rect.y, rect.x, rect.height, rect.width, view);
}

std::string explorer_footer(const SkillsBrowseCore& core, const std::string& last_status) {
    std::string footer;
    if (!core.marks().empty())
        footer = std::to_string(core.marks().size()) + " marked · i installs all · Space marks  ";
    footer +=
        core.error().kind != agent::IndexError::None ? failure_text(core.error()) : last_status;
    return footer;
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

void paint_explorer(WINDOW* w, const SkillsBrowseCore& core, const std::string& query,
                    const std::string& last_status,
                    const std::map<std::string, SkillDetailView>& details,
                    const std::string& budget) {
    const int aw = getmaxx(w);
    const int ah = getmaxy(w);
    werase(w);

    const std::string field = query.empty() ? "type to search (2+ chars)" : query;
    mvwaddnstr(w, 0, 0, ("/ " + field).c_str(), std::max(0, aw - 24));
    if (core.stale())
        mvwaddnstr(w, 0, std::max(0, aw - 8), "[cached]", 8);
    const std::string sort_label = std::string("sort: ") + sort_name(core.sort());
    mvwaddnstr(w, 0, std::max(0, aw - static_cast<int>(sort_label.size()) - 1), sort_label.c_str(),
               static_cast<int>(sort_label.size()));

    draw_tabs(w, 1, aw, core.tab());
    if (!budget.empty() && (core.tab() == SkillTab::Installed || core.tab() == SkillTab::Updates))
        mvwaddnstr(w, 1, std::max(0, aw - static_cast<int>(budget.size()) - 1), budget.c_str(),
                   static_cast<int>(budget.size()));
    mvwhline(w, 2, 0, ACS_HLINE, std::max(0, aw));

    const SkillsLayout layout = skills_layout(aw, ah);
    const int facet_w = layout.show_facets ? layout.facets_w : 0;
    const int list_x = facet_w;
    const int list_w = std::max(10, layout.list_w);
    const int detail_x = list_x + list_w + 2;
    const int detail_w = std::max(0, aw - detail_x - 1);
    const int top = 3;
    const int height = std::max(1, ah - 4);

    if (layout.show_facets)
        draw_facets(w, core, top, height, facet_w - 1);
    paint_rows(w, core, query, ListRect{list_x, top, list_w, height});
    const std::string footer = explorer_footer(core, last_status);
    if (!footer.empty())
        mvwaddnstr(w, top + height, 0, footer.c_str(), std::max(0, aw - 1));

    if (layout.show_detail)
        paint_detail_pane(w, core, details, ListRect{detail_x, top, detail_w, height});
    wnoutrefresh(w);
    update_panels();
    doupdate();
}

class SkillsExplorer {
public:
    SkillsExplorer(const SkillsExplorerOptions& options,
                   std::function<void(const std::string&)> status)
        : options_(options), status_(std::move(status)),
          dlg_(std::make_unique<Dialog>(std::max(10, options.rows - 2),
                                        std::max(50, options.cols - 2), "Skills")) {
        dlg_->set_footer({{"/", "search"},
                          {"Enter", "detail"},
                          {"i", "install"},
                          {"u", "update"},
                          {"U", "update all"},
                          {"d", "diff"},
                          {"?", "help"},
                          {"Esc", "close"}});
        keypad(dlg_->win(), TRUE);
        nodelay(dlg_->win(), TRUE);

        agent::SkillIndexOptions index_opts;
        index_opts.cache_root = options.cache_root;
        index_opts.token = options.token;
        if (options.index_disabled)
            index_opts.base_url.clear();
        else if (!options.index_base.empty())
            index_opts.base_url = options.index_base;
        agent::GithubOptions github_opts;
        github_opts.cache_root = options.cache_root;
        github_opts.token = options.github_token.empty() ? options.token : options.github_token;
        if (!options.github_api_base.empty())
            github_opts.api_base = options.github_api_base;
        if (!options.github_raw_base.empty())
            github_opts.raw_base = options.github_raw_base;
        service_ = std::make_unique<SkillsService>(make_skills_backends(index_opts, github_opts));
        life_ = std::make_unique<SkillsLifecycle>(
            SkillsLifecycleOptions{options.project_root, options.global_root, options.ledger,
                                   options.set_state},
            *service_);
        apply_initial_state();
    }

    void run() {
        while (true) {
            paint_explorer(dlg_->content(), core_, query_, last_status_, details_, options_.budget);
            const int ch = wgetch(dlg_->win());
            if (ch == ERR) {
                tick();
                napms(50);
                continue;
            }
            const SkillIntent intent = core_.key(ch, list_height());
            if (!apply(intent))
                return;
            tick();
        }
    }

private:
    int content_height() const { return dlg_->rows() - 2; }
    int list_height() const { return std::max(1, content_height() - 4); }

    void refresh_installed() {
        installed_rows_ = options_.installed ? options_.installed() : std::vector<SkillRow>{};
        installed_names_.clear();
        for (auto& row : installed_rows_) {
            installed_names_.insert(row.ref.name);
            row.update = life_->has_update(row.ref.name);
        }
        life_->set_installed(installed_rows_);
        core_.set_installed(installed_rows_);
    }

    // By value: `visible()` builds a fresh vector each call, so a pointer into
    // it would dangle the moment it returns.
    std::optional<SkillRow> find_row(const std::string& id) const {
        for (const auto& row : core_.visible()) {
            if (row.ref.id == id)
                return row;
        }
        for (const auto& row : installed_rows_) {
            if (row.ref.id == id)
                return row;
        }
        return std::nullopt;
    }

    std::optional<agent::SkillRef> find_ref(const std::string& id) const {
        for (const auto& row : core_.visible())
            if (row.ref.id == id)
                return row.ref;
        for (const auto& row : installed_rows_)
            if (row.ref.id == id)
                return row.ref;
        return std::nullopt;
    }

    SkillRow decorate(agent::SkillRef ref) const {
        SkillRow row;
        row.ref = std::move(ref);
        row.installed = installed_names_.count(row.ref.name) != 0;
        return row;
    }

    bool apply(const SkillIntent& intent);
    void help() const;
    bool layout_shows_facets() const;
    bool facet_overlay();
    void apply_initial_state();
    void apply_tab(SkillTab tab);
    void tick();
    void on_search(const SkillsEvent& event);
    void run_shelf(SkillTab tab);
    void run_collection(const std::string& name);
    bool open_detail(const std::string& id);
    bool apply_override(const SkillRow& row, bool block);
    bool shelf_for_you();
    bool shelf_collections();
    bool shelf_official();
    bool shelf_trending();
    void shelf_next();
    void shelf_done();
    void on_detail(const SkillsEvent& event);
    void on_install(const SkillsEvent& event);
    void prompt_install(const std::string& id);
    void install_marked(const std::vector<std::string>& ids);
    void on_batch_install(const SkillsEvent& event);
    std::string batch_summary() const;
    bool cancel_batch();
    bool close_or_cancel();
    void review_pending_update(const agent::SkillDetail& detail);
    bool install_batched(const agent::SkillDetail& detail);
    bool apply_lifecycle(const SkillIntent& intent);
    bool preview(const SkillRow& row);
    bool audit(const SkillRow& row);
    void remove_current(const SkillRow& row);
    void show_review(const agent::UpdateReview& review);
    void update_all();
    bool show_diff(const SkillRow& row);
    bool apply_facet_intent(const SkillIntent& intent);
    void update_current(const SkillRow& row);
    bool confirm(const std::string& title, const std::string& text,
                 const std::string& note = {}) const;

    SkillsExplorerOptions options_;
    std::function<void(const std::string&)> status_;
    std::unique_ptr<Dialog> dlg_;
    std::unique_ptr<SkillsService> service_;
    std::unique_ptr<SkillsLifecycle> life_;
    SkillsBrowseCore core_;
    std::vector<SkillRow> installed_rows_;
    std::set<std::string> installed_names_;
    std::map<std::string, SkillDetailView> details_;
    std::map<std::string, agent::SkillDetail> raw_details_;
    std::string query_;
    std::string pending_install_;
    std::string pending_update_;
    std::string pending_diff_;
    std::string pending_apply_;
    // Shelf runner: queries are asked one at a time (the service takes one at a
    // time) and their pages merge when the last lands.
    bool shelf_running_ = false;
    std::deque<std::string> shelf_queue_;
    std::vector<SkillRow> shelf_rows_;
    std::vector<ShelfPage> shelf_pages_;
    std::string shelf_reason_;
    std::set<std::string> previewed_;
    std::set<std::string> batch_pending_;
    std::set<std::string> batch_names_;
    int batch_total_ = 0;
    int batch_installed_ = 0;
    int batch_failed_ = 0;
    bool batch_cancel_ = false;
    std::vector<std::string> batch_skipped_;
    std::vector<std::string> batch_failures_;
    std::string pending_preview_;
    std::string last_status_;
    bool pending_search_ = false;
    Clock::time_point last_input_{};
};

void SkillsExplorer::apply_initial_state() {
    core_.set_sort(SkillSort::Relevance);
    core_.set_tab(options_.initial_tab);
    if (!options_.initial_query.empty()) {
        core_.set_filter(options_.initial_query);
        query_ = options_.initial_query;
        pending_search_ = true;
        last_input_ = Clock::now() - std::chrono::milliseconds(300);
    }
    if (!options_.initial_filter.empty()) {
        core_.set_filter(options_.initial_filter);
        query_ = options_.initial_filter;
    }
    refresh_installed();
    if (!options_.install_id.empty()) {
        pending_install_ = options_.install_id;
        service_->detail(ref_from_id(options_.install_id));
    }
}

bool SkillsExplorer::apply(const SkillIntent& intent) {
    switch (intent.kind) {
    case SkillIntent::Kind::Search:
        query_ = core_.filter();
        pending_search_ = true;
        last_input_ = Clock::now();
        return true;
    case SkillIntent::Kind::OpenDetail:
        return open_detail(intent.id);
    case SkillIntent::Kind::Install:
        prompt_install(intent.id);
        return true;
    case SkillIntent::Kind::InstallMarked:
        install_marked(intent.ids);
        return true;
    case SkillIntent::Kind::SetTab:
        apply_tab(intent.tab);
        return true;
    case SkillIntent::Kind::Refresh:
        pending_search_ = true;
        return true;

    case SkillIntent::Kind::Close:
        return close_or_cancel();
    case SkillIntent::Kind::Help:
    case SkillIntent::Kind::FocusFacets:
    case SkillIntent::Kind::ToggleFacet:
    case SkillIntent::Kind::ClearFacets:
        return apply_facet_intent(intent);
    case SkillIntent::Kind::None:
        return true;
    default:
        return apply_lifecycle(intent);
    }
}

bool SkillsExplorer::apply_facet_intent(const SkillIntent& intent) {
    if (intent.kind == SkillIntent::Kind::Help) {
        help();
        return true;
    }
    if (intent.kind == SkillIntent::Kind::FocusFacets && !layout_shows_facets())
        return facet_overlay();
    return true;
}

void SkillsExplorer::remove_current(const SkillRow& row) {
    const std::string root = life_->install_root(row);
    if (root.empty()) {
        last_status_ = "only project/global skills can be removed here";
        return;
    }
    const bool active = options_.is_active && options_.is_active(row.ref.name);
    if (!confirm("Remove skill", "Remove " + row.ref.name + " from " + root + "?",
                 active ? "active in this session until the conversation ends" : ""))
        return;
    last_status_ = life_->remove(row);
    details_.erase(row.ref.id);
    raw_details_.erase(row.ref.id);
    refresh_installed();
}

void SkillsExplorer::show_review(const agent::UpdateReview& review) {
    std::string body;
    for (const auto& line : agent::update_review_lines(review))
        body += line + "\n";
    run_body_preview(review.name + " update", body);
}

void SkillsExplorer::update_current(const SkillRow& row) {
    const std::string name = row.ref.name;
    if (options_.update_check_disabled) {
        last_status_ = "update checks are disabled (skills_update_check=never)";
        return;
    }
    if (const auto review = life_->review_for(name)) {
        // First press: show what would change. Second press: apply, and for a
        // locally edited skill that second press is the explicit override.
        if (options_.update_review_required && pending_apply_ != name) {
            show_review(*review);
            pending_apply_ = name;
            last_status_ = review->needs_override()
                               ? "local edits · u again overrides and replaces them"
                               : "review closed · u applies";
            return;
        }
        pending_apply_.clear();
        last_status_ = life_->apply_update(row, review->needs_override());
        refresh_installed();
        return;
    }
    agent::SkillRef ref;
    const UpdateAction action = life_->prepare_update(row, ref, last_status_);
    if (action == UpdateAction::Fetch) {
        pending_update_ = name;
        service_->detail(ref);
    } else if (action == UpdateAction::Apply) {
        last_status_ = life_->apply_update(row);
        refresh_installed();
    }
}

bool SkillsExplorer::show_diff(const SkillRow& row) {
    if (options_.update_check_disabled && !life_->review_for(row.ref.name)) {
        last_status_ = "update checks are disabled (skills_update_check=never)";
        return true;
    }
    if (const auto review = life_->review_for(row.ref.name)) {
        show_review(*review);
        last_status_ = review->needs_override() ? "local edits · u overrides and replaces them"
                                                : "diff closed · u applies";
        return true;
    }
    agent::SkillRef ref;
    const UpdateAction action = life_->prepare_update(row, ref, last_status_);
    if (action == UpdateAction::Fetch) {
        pending_diff_ = row.ref.name;
        service_->detail(ref);
    } else if (action == UpdateAction::None) {
        return true;
    }
    return true;
}

void SkillsExplorer::update_all() {
    last_status_ = life_->apply_all(installed_rows_);
    pending_apply_.clear();
    refresh_installed();
}

bool SkillsExplorer::apply_lifecycle(const SkillIntent& intent) {
    const auto row = core_.current();
    if (!row) {
        last_status_ = "no skill selected";
        return true;
    }
    if (row->collection) {
        last_status_ = "that is an editorial shelf; Enter opens the skills it lists";
        return true;
    }
    switch (intent.kind) {
    case SkillIntent::Kind::Adopt:
        last_status_ = life_->adopt(*row);
        refresh_installed();
        return true;
    case SkillIntent::Kind::Remove:
        remove_current(*row);
        return true;
    case SkillIntent::Kind::Update:
        update_current(*row);
        return true;
    case SkillIntent::Kind::UpdateAll:
        update_all();
        return true;
    case SkillIntent::Kind::Diff:
        return show_diff(*row);
    case SkillIntent::Kind::ToggleEnabled:
        return apply_override(*row, false);
    case SkillIntent::Kind::Block:
        return apply_override(*row, true);
    case SkillIntent::Kind::Preview:
        return preview(*row);
    case SkillIntent::Kind::Audit:
        return audit(*row);
    default:
        last_status_ = "not available yet";
        return true;
    }
}

bool SkillsExplorer::preview(const SkillRow& row) {
    const auto raw = raw_details_.find(row.ref.id);
    if (raw == raw_details_.end()) {
        pending_preview_ = row.ref.id;
        if (const auto ref = find_ref(row.ref.id))
            service_->detail(*ref);
        last_status_ = "loading body...";
        return true;
    }
    previewed_.insert(row.ref.name);
    run_body_preview(row.ref.name, raw->second.body);
    last_status_ = "preview closed";
    return true;
}

bool SkillsExplorer::audit(const SkillRow& row) {
    const auto raw = raw_details_.find(row.ref.id);
    if (raw == raw_details_.end()) {
        last_status_ = "load details first (Enter)";
        return true;
    }
    const auto risks = agent::scan_skill_body(raw->second.body);
    int shell = 0;
    int network = 0;
    for (const auto& signal : risks) {
        if (signal.kind == "shell")
            ++shell;
        else if (signal.kind == "network")
            ++network;
    }
    last_status_ = "not audited \u00b7 " + std::to_string(risks.size()) + " signals (" +
                   std::to_string(shell) + " shell, " + std::to_string(network) + " network)";
    return true;
}

bool SkillsExplorer::shelf_for_you() {
    const agent::ProjectFacts facts =
        agent::detect_project_signals(options_.project_signals, options_.project_root);
    if (facts.labels.empty()) {
        last_status_ = "no project signals here";
        return false;
    }
    shelf_reason_ = "Based on: ";
    for (std::size_t i = 0; i < facts.labels.size(); ++i) {
        if (i > 0)
            shelf_reason_ += ", ";
        shelf_reason_ += facts.labels[i];
    }
    for (const auto& query : facts.queries)
        shelf_queue_.push_back(query);
    return true;
}

bool SkillsExplorer::shelf_collections() {
    // Editorial shelves are rows in their own right; Enter runs a collection's
    // queries, so nothing here claims to be a registry category.
    std::vector<SkillRow> rows;
    for (const auto& collection : options_.collections) {
        SkillRow row;
        row.ref.id = "collection:" + collection.name;
        row.ref.name = collection.name;
        row.ref.source = "editorial";
        row.description = collection.description;
        row.collection = true;
        rows.push_back(std::move(row));
    }
    core_.set_rows(std::move(rows));
    shelf_reason_ = "editorial collections, not registry categories";
    last_status_ = "collections";
    return false;
}

bool SkillsExplorer::shelf_official() {
    for (const auto& owner : options_.owners)
        shelf_queue_.push_back(owner);
    shelf_reason_ = "curated owners";
    return !shelf_queue_.empty();
}

bool SkillsExplorer::shelf_trending() {
    if (query_.empty()) {
        last_status_ = "no trend signal: type a query, or use Top";
        return false;
    }
    shelf_queue_.push_back(query_);
    shelf_reason_ = "no trend signal: sorted by installs";
    return true;
}

bool SkillsExplorer::open_detail(const std::string& id) {
    if (id.rfind("collection:", 0) == 0) {
        run_collection(id.substr(std::string("collection:").size()));
        return true;
    }
    if (const auto ref = find_ref(id))
        service_->detail(*ref);
    last_status_ = "loading details...";
    return true;
}

bool SkillsExplorer::apply_override(const SkillRow& row, bool block) {
    last_status_ = block ? life_->block(row) : life_->toggle(row);
    refresh_installed();
    return true;
}

void SkillsExplorer::run_collection(const std::string& name) {
    for (const auto& collection : options_.collections) {
        if (collection.name != name)
            continue;
        shelf_queue_.clear();
        shelf_pages_.clear();
        for (const auto& query : collection.queries)
            shelf_queue_.push_back(query);
        shelf_reason_ = "collection: " + collection.name;
        if (shelf_queue_.empty()) {
            last_status_ = "that collection has ids only; install them by name";
            return;
        }
        shelf_running_ = true;
        last_status_ = "loading " + collection.name + "...";
        shelf_next();
        return;
    }
    last_status_ = "unknown collection";
}

void SkillsExplorer::run_shelf(SkillTab tab) {
    shelf_queue_.clear();
    shelf_pages_.clear();
    shelf_reason_.clear();
    bool loads = false;
    switch (tab) {
    case SkillTab::ForYou:
        loads = shelf_for_you();
        break;
    case SkillTab::Collections:
        loads = shelf_collections();
        break;
    case SkillTab::Official:
        loads = shelf_official();
        break;
    case SkillTab::Trending:
        loads = shelf_trending();
        break;
    default:
        return;
    }
    if (!loads || shelf_queue_.empty())
        return;
    shelf_running_ = true;
    last_status_ = std::string("loading ") + skill_tab_name(tab) + "...";
    shelf_next();
}

void SkillsExplorer::shelf_next() {
    if (shelf_queue_.empty()) {
        shelf_done();
        return;
    }
    const std::string query = shelf_queue_.front();
    shelf_queue_.pop_front();
    service_->search(query, 20);
}

void SkillsExplorer::shelf_done() {
    shelf_running_ = false;
    bool stale = false;
    agent::IndexFailure error;
    std::vector<SkillRow> merged = merge_shelf_rows(shelf_pages_, stale, error);
    if (core_.sort() == SkillSort::Installs) {
        std::stable_sort(merged.begin(), merged.end(), [](const SkillRow& a, const SkillRow& b) {
            return a.ref.installs > b.ref.installs;
        });
    }
    core_.set_rows(std::move(merged), stale, error);
    core_.set_query_live(false);
    last_status_ =
        shelf_reason_.empty() ? std::to_string(core_.visible_count()) + " results" : shelf_reason_;
}

void SkillsExplorer::apply_tab(SkillTab tab) {
    core_.set_tab(tab);
    if (tab == SkillTab::Installed) {
        refresh_installed();
        last_status_ = "installed skills";
    } else if (tab == SkillTab::Updates) {
        refresh_installed();
        last_status_ = "updates: u checks, u again applies";
    } else if (tab == SkillTab::Top) {
        pending_search_ = !query_.empty();
        last_status_ = "top skills";
    } else {
        run_shelf(tab);
    }
}

bool SkillsExplorer::layout_shows_facets() const {
    return skills_layout(options_.cols, options_.rows).show_facets;
}

// Narrow terminals get the same facet state in a modal: the column would eat
// the list, and hiding the filters entirely is the one thing the spec forbids.
bool SkillsExplorer::facet_overlay() {
    if (core_.facet_groups().empty()) {
        last_status_ = "no filters to show yet";
        return true;
    }
    Dialog dlg(std::min(18, options_.rows - 2), std::min(56, options_.cols - 4), "Filters");
    dlg.set_footer({{"j/k", "move"}, {"Enter", "toggle"}, {"r", "reset"}, {"Esc", "close"}});
    WINDOW* w = dlg.content();
    dlg.show();
    while (true) {
        werase(w);
        draw_facets(w, core_, 0, dlg.rows() - 2, dlg.cols() - 2);
        wrefresh(w);
        const int ch = wgetch(w);
        if (ch == ERR)
            continue;
        const SkillIntent intent = core_.facet_key(ch);
        if (intent.kind == SkillIntent::Kind::Close)
            return true;
        if (core_.pane() != SkillPane::Facets) {
            core_.set_pane(SkillPane::Facets); // stay in the overlay until Esc
            if (ch == 27)
                return true;
        }
    }
}

void SkillsExplorer::help() const {
    Dialog dlg(std::min(18, options_.rows - 2), std::min(64, options_.cols - 4), "Skills keys");
    dlg.set_footer({{"Esc", "close"}});
    WINDOW* w = dlg.content();
    const char* lines[] = {
        "/          focus the search field",
        "Enter      leave search / load the selected detail",
        "i          install sheet (review gate)",
        "p          preview the body",
        "v          audit readout (risk signals)",
        "u          review an update; u again applies",
        "U          apply every reviewed update",
        "d          diff installed vs remote",
        "D          remove the selected skill",
        "x / b      disable / block",
        "A          adopt an unmanaged directory",
        "Space      mark a row; a marks the visible ones",
        "i          install the marked rows, or open the sheet",
        "1..7       shelves (Top, Installed, Updates have data)",
        "s          cycle sort: relevance, installs, name",
        "Tab        move focus between panes",
        "?          this help; Esc closes",
    };
    int y = 0;
    for (const char* line : lines) {
        if (y >= dlg.rows() - 2)
            break;
        mvwaddnstr(w, y++, 0, line, dlg.cols() - 2);
    }
    dlg.show();
    while (true) {
        const int ch = wgetch(w);
        if (ch == 27 || ch == 'q' || ch == '?')
            return;
    }
}

void SkillsExplorer::tick() {
    if (pending_search_ && since(last_input_) >= 250) {
        pending_search_ = false;
        // The installed shelf filters locally; a registry search there would
        // spend a request on rows the shelf never shows.
        if (query_.size() >= 2 && core_.tab() != SkillTab::Installed)
            service_->search(query_);
        else
            core_.set_rows({});
    }
    for (auto& event : service_->drain()) {
        if (event.kind == SkillsEvent::Kind::SearchDone)
            on_search(event);
        else if (event.kind == SkillsEvent::Kind::DetailDone)
            on_detail(event);
        else if (event.kind == SkillsEvent::Kind::InstallDone)
            on_install(event);
    }
}

void SkillsExplorer::on_search(const SkillsEvent& event) {
    if (shelf_running_) {
        ShelfPage page;
        if (event.search.page) {
            page.stale = event.search.page->cached;
            for (const auto& ref : event.search.page->skills)
                page.rows.push_back(decorate(ref));
        } else {
            page.error = event.search.error;
        }
        shelf_pages_.push_back(std::move(page));
        shelf_next();
        return;
    }
    if (event.query != query_)
        return;
    if (!event.search.page) {
        core_.set_rows({}, false, event.search.error);
        last_status_ = failure_text(event.search.error);
        return;
    }
    std::vector<SkillRow> rows;
    rows.reserve(event.search.page->skills.size());
    for (const auto& ref : event.search.page->skills)
        rows.push_back(decorate(ref));
    core_.set_rows(std::move(rows), event.search.page->cached);
    last_status_ = std::to_string(event.search.page->skills.size()) + " results";
}

void SkillsExplorer::review_pending_update(const agent::SkillDetail& detail) {
    const std::string name = detail.ref.name;
    const bool wants_diff = pending_diff_ == name;
    pending_update_.clear();
    pending_diff_.clear();
    const SkillRow* row = installed_row(installed_rows_, name);
    if (!row)
        return;
    last_status_ = life_->check_update(*row, detail);
    refresh_installed();
    const auto review = life_->review_for(name);
    if (review && (wants_diff || options_.update_review_required)) {
        show_review(*review);
        pending_apply_ = name;
        last_status_ = review->needs_override() ? "local edits · u overrides and replaces them"
                                                : "review closed · u applies";
    }
}

bool SkillsExplorer::install_batched(const agent::SkillDetail& detail) {
    if (batch_pending_.erase(detail.ref.id) == 0)
        return false;
    const std::string root = options_.install_global ? options_.global_root : options_.project_root;
    service_->install(detail, agent::skill_slug(detail.ref.id), root, false, true);
    last_status_ = "installing " + detail.ref.name + "...";
    return true;
}

void SkillsExplorer::on_detail(const SkillsEvent& event) {
    if (!event.detail.detail) {
        last_status_ = failure_text(event.detail.error);
        return;
    }
    const agent::SkillDetail& detail = *event.detail.detail;
    details_[detail.ref.id] = view_of(detail);
    raw_details_[detail.ref.id] = detail;
    core_.set_row_category(detail.ref.id, detail.category);
    last_status_ = "loaded " + detail.ref.name;
    if (pending_preview_ == detail.ref.id) {
        pending_preview_.clear();
        run_body_preview(detail.ref.name, detail.body);
        last_status_ = "preview closed";
        return;
    }
    if (pending_update_ == detail.ref.name || pending_diff_ == detail.ref.name) {
        review_pending_update(detail);
        return;
    }
    if (install_batched(detail))
        return;
    if (pending_install_ == detail.ref.id) {
        pending_install_.clear();
        prompt_install(detail.ref.id);
    }
}

void SkillsExplorer::on_install(const SkillsEvent& event) {
    if (batch_total_ > 0) {
        on_batch_install(event);
        return;
    }
    if (!event.install.ok) {
        last_status_ = "install failed: " + event.install.error;
        return;
    }
    last_status_ = "installed " + event.name + " · " + std::to_string(event.install.files) +
                   " files" + (event.ledger_saved ? "" : " (ledger not saved)");
    refresh_installed();
}

void SkillsExplorer::install_marked(const std::vector<std::string>& ids) {
    batch_pending_.clear();
    batch_names_.clear();
    batch_skipped_.clear();
    batch_total_ = 0;
    batch_installed_ = 0;
    batch_failed_ = 0;
    batch_cancel_ = false;
    batch_failures_.clear();
    const std::string root = options_.install_global ? options_.global_root : options_.project_root;
    std::vector<BatchCandidate> candidates;
    for (const auto& id : ids) {
        BatchCandidate candidate;
        candidate.id = id;
        candidate.name = agent::skill_slug(id);
        candidate.previewed = previewed_.count(candidate.name) != 0;
        candidate.exists_in_scope = std::filesystem::exists(root + "/" + candidate.name);
        if (const auto row = find_row(id))
            candidate.official = row->official;
        candidates.push_back(std::move(candidate));
    }
    const BatchPlan plan = plan_batch(candidates, options_.review_required);
    batch_skipped_ = plan.skipped;
    for (const auto& id : plan.install_ids) {
        const auto ref = find_ref(id);
        if (!ref)
            continue;
        batch_pending_.insert(id);
        batch_names_.insert(agent::skill_slug(id));
        ++batch_total_;
        service_->detail(*ref);
    }
    last_status_ =
        plan.install_ids.empty()
            ? batch_summary()
            : "installing " + std::to_string(plan.install_ids.size()) + " marked skills...";
}

std::string SkillsExplorer::batch_summary() const {
    return batch_report(batch_installed_, batch_skipped_, batch_failed_, batch_failures_,
                        batch_cancel_);
}

bool SkillsExplorer::close_or_cancel() {
    if (cancel_batch())
        return true;
    status_("skills closed");
    return false;
}

bool SkillsExplorer::cancel_batch() {
    if (batch_total_ <= 0)
        return false;
    batch_cancel_ = true;
    batch_pending_.clear(); // queued details no longer install
    batch_names_.clear();
    batch_total_ = 0;
    last_status_ = batch_summary();
    return true;
}

void SkillsExplorer::on_batch_install(const SkillsEvent& event) {
    if (batch_names_.count(event.name) == 0)
        return;
    batch_names_.erase(event.name);
    if (event.install.ok) {
        ++batch_installed_;
    } else {
        ++batch_failed_;
        batch_failures_.emplace_back(event.name + " (" + event.install.error + ")");
    }
    if (!batch_names_.empty())
        return;
    batch_total_ = 0;
    last_status_ = batch_summary();
    refresh_installed();
}

void SkillsExplorer::prompt_install(const std::string& id) {
    if (const auto row = find_row(id); row && row->collection) {
        last_status_ = "that is an editorial shelf; Enter opens the skills it lists";
        return;
    }
    const auto raw = raw_details_.find(id);
    if (raw == raw_details_.end()) {
        pending_install_ = id;
        if (const auto ref = find_ref(id))
            service_->detail(*ref);
        last_status_ = "loading skill...";
        return;
    }
    const std::string name = agent::skill_slug(id);
    InstallSheetOptions sheet_options;
    sheet_options.project_exists = std::filesystem::exists(options_.project_root + "/" + name);
    sheet_options.global_exists = std::filesystem::exists(options_.global_root + "/" + name);
    sheet_options.review_required = options_.review_required;
    sheet_options.audit_required = options_.audit_required;
    sheet_options.default_global = options_.install_global;
    const InstallDecision decision =
        run_install_sheet(evidence_of(raw->second, name), raw->second.body, sheet_options);
    touchwin(dlg_->win());
    wrefresh(dlg_->win());
    if (!decision.confirmed)
        return;
    const std::string root =
        decision.scope == InstallScope::Project ? options_.project_root : options_.global_root;
    service_->install(raw->second, name, root, decision.replace, decision.enabled);
    last_status_ = "installing " + name + "...";
}

bool SkillsExplorer::confirm(const std::string& title, const std::string& text,
                             const std::string& note) const {
    Dialog dlg(std::min(10, options_.rows - 2), std::min(68, options_.cols - 4), title);
    dlg.set_footer({{"y", "confirm"}, {"n", "cancel"}});
    WINDOW* w = dlg.content();
    mvwaddnstr(w, 1, 0, text.c_str(), dlg.cols() - 2);
    if (!note.empty())
        mvwaddnstr(w, 3, 0, note.c_str(), dlg.cols() - 2);
    dlg.show();
    keypad(w, TRUE);
    while (true) {
        const int ch = wgetch(w);
        if (ch == 'y' || ch == 'Y')
            return true;
        if (ch == 'n' || ch == 'N' || ch == 27 || ch == 'q')
            return false;
    }
}

} // namespace

void run_skills_explorer(const SkillsExplorerOptions& options,
                         const std::function<void(const std::string&)>& status) {
    if (options.rows < 12 || options.cols < 50) {
        status("skills: terminal too small (need 50x12)");
        return;
    }
    SkillsExplorer(options, status).run();
}

} // namespace tui
