#include "tui/skills_explorer.h"

#include "agent/github_index.h"
#include "tui/dialog.h"
#include "tui/skills_service.h"

#include <algorithm>
#include <chrono>
#include <functional>
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
        return "offline";
    case agent::IndexError::Timeout:
        return "timed out";
    case agent::IndexError::Cancelled:
        return "cancelled";
    case agent::IndexError::RateLimited:
        return "rate limited" + (error.retry_after_s > 0
                                     ? " (retry in " + std::to_string(error.retry_after_s) + "s)"
                                     : "");
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

void draw_tabs(WINDOW* w, int row, int width, SkillTab tab) {
    const std::pair<SkillTab, const char*> tabs[] = {{SkillTab::Top, "Top"},
                                                     {SkillTab::Installed, "Installed"}};
    int x = 0;
    for (const auto& [kind, label] : tabs) {
        if (kind == tab)
            wattron(w, A_REVERSE);
        mvwaddnstr(w, row, x, label, std::max(0, width - x));
        if (kind == tab)
            wattroff(w, A_REVERSE);
        x += static_cast<int>(std::string(label).size()) + 3;
    }
}

void draw_row(WINDOW* w, int y, int width, const SkillRow& row, bool selected) {
    std::string badges;
    if (row.installed)
        badges += "*";
    if (row.update)
        badges += "+";

    std::string name = row.ref.name;
    if (name.size() > 26) {
        name.resize(25);
        name += "\u2026";
    }

    const std::string tail = compact_installs(row.ref.installs);
    const int tail_x = std::max(28, width - static_cast<int>(tail.size()) - 1);

    if (selected)
        wattron(w, A_REVERSE);
    mvwhline(w, y, 0, ' ', std::max(0, width));
    mvwaddnstr(w, y, 0, name.c_str(), 27);
    if (!badges.empty())
        mvwaddnstr(w, y, 28, badges.c_str(), 4);
    if (width > 40)
        mvwaddnstr(w, y, 32, row.ref.source.c_str(), std::max(0, tail_x - 33));
    mvwaddnstr(w, y, tail_x, tail.c_str(), static_cast<int>(tail.size()));
    if (selected)
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

int choose_scope(int rows, int cols) {
    Dialog dlg(std::min(10, rows - 2), std::min(58, cols - 4), "Install skill");
    dlg.set_footer({{"1", "project"}, {"2", "global"}, {"Esc", "cancel"}});
    WINDOW* w = dlg.content();
    const int aw = dlg.cols() - 2;
    mvwaddnstr(w, 1, 0, "Install for this project, or for every project?", aw);
    mvwaddnstr(w, 3, 0, "project  .amber/skills", aw);
    mvwaddnstr(w, 4, 0, "global   ~/.config/amber/skills", aw);
    keypad(w, TRUE);
    while (true) {
        const int ch = wgetch(w);
        if (ch == '1')
            return 1;
        if (ch == '2')
            return 2;
        if (ch == 27 || ch == 'q')
            return 0;
    }
}

void paint_rows(WINDOW* w, const SkillsBrowseCore& core, const std::string& query, int top,
                int height, int list_w) {
    const auto rows_now = core.visible();
    for (int i = 0; i < height; ++i) {
        const int index = core.scroll_offset() + i;
        if (index >= static_cast<int>(rows_now.size()))
            break;
        draw_row(w, top + i, list_w, rows_now[static_cast<size_t>(index)],
                 index == core.selection());
    }
    if (rows_now.empty())
        mvwaddnstr(w, top, 0, query.empty() ? "Type a query to search." : "No matches.",
                   std::max(0, list_w - 1));
}

void paint_explorer(WINDOW* w, const SkillsBrowseCore& core, const std::string& query,
                    const std::string& last_status,
                    const std::map<std::string, SkillDetailView>& details) {
    const int aw = getmaxx(w);
    const int ah = getmaxy(w);
    werase(w);

    const std::string field = query.empty() ? "type to search (2+ chars)" : query;
    mvwaddnstr(w, 0, 0, ("/ " + field).c_str(), std::max(0, aw - 12));
    if (core.stale())
        mvwaddnstr(w, 0, std::max(0, aw - 8), "[cached]", 8);
    draw_tabs(w, 1, aw, core.tab());
    mvwhline(w, 2, 0, ACS_HLINE, std::max(0, aw));

    const int list_w = aw >= 80 ? std::min(46, aw - 24) : aw;
    const int top = 3;
    const int height = std::max(1, ah - 4);
    paint_rows(w, core, query, top, height, list_w);
    const std::string footer =
        core.error().kind != agent::IndexError::None ? failure_text(core.error()) : last_status;
    if (!footer.empty())
        mvwaddnstr(w, top + height, 0, footer.c_str(), std::max(0, aw - 1));

    if (aw >= 80) {
        std::optional<SkillDetailView> view;
        if (const auto current = core.current()) {
            const auto it = details.find(current->ref.id);
            if (it != details.end())
                view = it->second;
        }
        draw_detail(w, top, list_w + 2, height, aw - list_w - 3, view);
    }
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
                          {"1/2", "tab"},
                          {"Esc", "close"}});
        keypad(dlg_->win(), TRUE);
        timeout(100);

        agent::SkillIndexOptions index_opts;
        index_opts.cache_root = options.cache_root;
        index_opts.token = options.token;
        agent::GithubOptions github_opts;
        github_opts.cache_root = options.cache_root;
        github_opts.token = options.token;
        service_ = std::make_unique<SkillsService>(make_skills_backends(index_opts, github_opts));
        core_.set_sort(SkillSort::Relevance);
        refresh_installed();
    }

    void run() {
        while (true) {
            paint_explorer(dlg_->content(), core_, query_, last_status_, details_);
            const int ch = wgetch(dlg_->win());
            if (ch != ERR) {
                const SkillIntent intent = core_.key(ch, list_height());
                if (!apply(intent))
                    return;
            }
            tick();
        }
    }

private:
    int content_height() const { return dlg_->rows() - 2; }
    int list_height() const { return std::max(1, content_height() - 4); }

    void refresh_installed() {
        installed_rows_ = options_.installed ? options_.installed() : std::vector<SkillRow>{};
        installed_names_.clear();
        for (const auto& row : installed_rows_)
            installed_names_.insert(row.ref.name);
        core_.set_installed(installed_rows_);
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

    bool apply(const SkillIntent& intent) {
        switch (intent.kind) {
        case SkillIntent::Kind::Search:
            query_ = core_.filter();
            pending_search_ = true;
            last_input_ = Clock::now();
            return true;
        case SkillIntent::Kind::OpenDetail:
            if (const auto ref = find_ref(intent.id))
                service_->detail(*ref);
            last_status_ = "loading details...";
            return true;
        case SkillIntent::Kind::Install:
            prompt_install(intent.id);
            return true;
        case SkillIntent::Kind::SetTab:
            apply_tab(intent.tab);
            return true;
        case SkillIntent::Kind::Refresh:
            pending_search_ = true;
            return true;
        case SkillIntent::Kind::Close:
            status_("skills closed");
            return false;
        case SkillIntent::Kind::None:
            return true;
        default:
            last_status_ = "not available yet";
            return true;
        }
    }

    void apply_tab(SkillTab tab) {
        if (tab == SkillTab::Installed) {
            refresh_installed();
            last_status_ = "installed skills";
        } else if (tab == SkillTab::Top) {
            pending_search_ = !query_.empty();
        } else {
            core_.set_tab(SkillTab::Top);
            last_status_ = "that shelf is not wired yet";
        }
    }

    void tick() {
        if (pending_search_ && since(last_input_) >= 250) {
            pending_search_ = false;
            if (query_.size() >= 2)
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

    void on_search(const SkillsEvent& event) {
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

    void on_detail(const SkillsEvent& event) {
        if (!event.detail.detail) {
            last_status_ = failure_text(event.detail.error);
            return;
        }
        const agent::SkillDetail& detail = *event.detail.detail;
        details_[detail.ref.id] = view_of(detail);
        raw_details_[detail.ref.id] = detail;
        last_status_ = "loaded " + detail.ref.name;
        if (pending_install_ == detail.ref.id) {
            pending_install_.clear();
            prompt_install(detail.ref.id);
        }
    }

    void on_install(const SkillsEvent& event) {
        if (!event.install.ok) {
            last_status_ = "install failed: " + event.install.error;
            return;
        }
        last_status_ = "installed " + event.name + " · " + std::to_string(event.install.files) +
                       " files" + (event.ledger_saved ? "" : " (ledger not saved)");
        refresh_installed();
    }

    void prompt_install(const std::string& id) {
        const auto raw = raw_details_.find(id);
        if (raw == raw_details_.end()) {
            pending_install_ = id;
            if (const auto ref = find_ref(id))
                service_->detail(*ref);
            last_status_ = "loading skill...";
            return;
        }
        const int scope = choose_scope(options_.rows, options_.cols);
        touchwin(dlg_->win());
        wrefresh(dlg_->win());
        if (scope == 0)
            return;
        service_->install(raw->second, agent::skill_slug(id),
                          scope == 1 ? options_.project_root : options_.global_root, false, true);
        last_status_ = "installing " + agent::skill_slug(id) + "...";
    }

    SkillsExplorerOptions options_;
    std::function<void(const std::string&)> status_;
    std::unique_ptr<Dialog> dlg_;
    std::unique_ptr<SkillsService> service_;
    SkillsBrowseCore core_;
    std::vector<SkillRow> installed_rows_;
    std::set<std::string> installed_names_;
    std::map<std::string, SkillDetailView> details_;
    std::map<std::string, agent::SkillDetail> raw_details_;
    std::string query_;
    std::string pending_install_;
    std::string last_status_;
    bool pending_search_ = false;
    Clock::time_point last_input_{};
};

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
