#include "tui/skills_browse_core.h"
#include "tui/keys.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace tui {

namespace {

bool ci_contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    return it != haystack.end();
}

bool is_printable(int ch) {
    return ch >= 32 && ch < 127;
}

SkillIntent intent(SkillIntent::Kind kind) {
    SkillIntent i;
    i.kind = kind;
    return i;
}

SkillIntent with_id(SkillIntent::Kind kind, const std::string& id) {
    SkillIntent i;
    i.kind = kind;
    i.id = id;
    return i;
}

} // namespace

SkillsLayout skills_layout(int cols, int rows) {
    SkillsLayout l;
    if (cols >= 100) {
        l.facets_w = 24;
        l.list_w = 40;
        l.detail_w = std::max(0, cols - l.facets_w - l.list_w);
        l.show_facets = true;
        l.show_detail = true;
    } else if (cols >= 60) {
        l.list_w = 34;
        l.detail_w = std::max(0, cols - l.list_w);
        l.show_detail = true;
    } else {
        l.list_w = std::max(0, cols);
        l.detail_w = 0;
    }
    if (rows < 10) {
        l.show_detail = false;
        l.show_facets = false;
        l.list_w = std::max(0, cols);
        l.detail_w = 0;
    }
    return l;
}

SkillsBrowseCore::SkillsBrowseCore() = default;

void SkillsBrowseCore::set_rows(std::vector<SkillRow> rows, bool stale, agent::IndexFailure error) {
    rows_ = std::move(rows);
    stale_ = stale;
    error_ = std::move(error);
    reset_cursor();
}

void SkillsBrowseCore::set_row_category(const std::string& id, std::string category) {
    for (auto& row : rows_) {
        if (row.ref.id == id) {
            row.category = std::move(category);
            return;
        }
    }
}

void SkillsBrowseCore::set_installed(std::vector<SkillRow> installed) {
    installed_ = std::move(installed);
}

void SkillsBrowseCore::set_status(std::string status) {
    status_ = std::move(status);
}

void SkillsBrowseCore::set_pane(SkillPane pane) {
    pane_ = pane;
}

void SkillsBrowseCore::set_tab(SkillTab tab) {
    if (pane_ == SkillPane::Facets)
        pane_ = SkillPane::List;
    tab_ = tab;
    reset_cursor();
}

void SkillsBrowseCore::set_sort(SkillSort sort) {
    sort_ = sort;
    reset_cursor();
}

void SkillsBrowseCore::set_filter(std::string filter) {
    filter_ = std::move(filter);
    reset_cursor();
}

void SkillsBrowseCore::clear_marks() {
    marked_.clear();
}

void SkillsBrowseCore::scroll_detail(int delta) {
    detail_scroll_ = std::max(0, detail_scroll_ + delta);
}

bool SkillsBrowseCore::matches(const SkillRow& row) const {
    const bool text = ci_contains(row.ref.name, filter_) || ci_contains(row.ref.source, filter_) ||
                      ci_contains(row.description, filter_) || ci_contains(row.category, filter_);
    return text && passes_facets(row);
}

bool SkillsBrowseCore::passes_facets(const SkillRow& row) const {
    // AND across groups: every selected group must accept the row.
    return std::all_of(facets_.begin(), facets_.end(), [&row](const auto& entry) {
        return entry.second.empty() || entry.second.count(facet_value(row, entry.first)) != 0;
    });
}

bool SkillsBrowseCore::passes_facets_except(const SkillRow& row, FacetGroup skip) const {
    return std::all_of(facets_.begin(), facets_.end(), [&row, skip](const auto& entry) {
        if (entry.first == skip || entry.second.empty())
            return true;
        return entry.second.count(facet_value(row, entry.first)) != 0;
    });
}

const char* facet_group_name(FacetGroup group) noexcept {
    switch (group) {
    case FacetGroup::Status:
        return "status";
    case FacetGroup::Trust:
        return "trust";
    case FacetGroup::Category:
        return "category";
    case FacetGroup::Source:
        return "source";
    }
    return "?";
}

std::string facet_value(const SkillRow& row, FacetGroup group) {
    switch (group) {
    case FacetGroup::Status:
        if (!row.installed)
            return "not installed";
        if (!row.state.empty())
            return row.state;
        return "installed";
    case FacetGroup::Trust:
        return agent::trust_tier_name(row.tier);
    case FacetGroup::Category:
        return row.category.empty() ? "uncategorised" : row.category;
    case FacetGroup::Source:
        return row.ref.source.empty() ? "unknown" : row.ref.source;
    }
    return {};
}

void SkillsBrowseCore::toggle_facet(FacetGroup group, const std::string& value) {
    std::set<std::string>& values = facets_[group];
    if (values.count(value) != 0)
        values.erase(value);
    else
        values.insert(value);
    reset_cursor();
}

void SkillsBrowseCore::clear_facets() {
    facets_.clear();
    reset_cursor();
}

bool SkillsBrowseCore::facet_selected(FacetGroup group, const std::string& value) const {
    const auto it = facets_.find(group);
    return it != facets_.end() && it->second.count(value) != 0;
}

bool SkillsBrowseCore::any_facet() const noexcept {
    return std::any_of(facets_.begin(), facets_.end(),
                       [](const auto& entry) { return !entry.second.empty(); });
}

std::vector<std::pair<std::string, int>> SkillsBrowseCore::facet_counts(FacetGroup group) const {
    const bool installed_shelf = tab_ == SkillTab::Installed || tab_ == SkillTab::Updates;
    const auto& source = installed_shelf ? installed_ : rows_;
    std::map<std::string, int> counts;
    for (const auto& row : source) {
        if (tab_ == SkillTab::Updates && !row.update)
            continue;
        // The text filter applies; the other facets do too, so counts compose.
        const bool text =
            ci_contains(row.ref.name, filter_) || ci_contains(row.ref.source, filter_) ||
            ci_contains(row.description, filter_) || ci_contains(row.category, filter_);
        if (!text || !passes_facets_except(row, group))
            continue;
        if (facet_value(row, group).empty())
            continue;
        ++counts[facet_value(row, group)];
    }
    std::vector<std::pair<std::string, int>> out(counts.begin(), counts.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    });
    return out;
}

std::vector<FacetGroup> SkillsBrowseCore::facet_groups() const {
    static constexpr FacetGroup kGroups[] = {FacetGroup::Status, FacetGroup::Trust,
                                             FacetGroup::Category, FacetGroup::Source};
    std::vector<FacetGroup> groups;
    for (FacetGroup group : kGroups) {
        if (!facet_counts(group).empty())
            groups.push_back(group);
    }
    return groups;
}

std::string SkillsBrowseCore::facet_cursor_value() const {
    const auto counts = facet_counts(facet_cursor_.group);
    if (facet_cursor_.index >= counts.size())
        return {};
    return counts[facet_cursor_.index].first;
}

void SkillsBrowseCore::move_facet_cursor(int delta) {
    const std::vector<FacetGroup> groups = facet_groups();
    if (groups.empty())
        return;
    const auto it = std::find(groups.begin(), groups.end(), facet_cursor_.group);
    int group_index = it == groups.end() ? 0 : static_cast<int>(it - groups.begin());
    int index = static_cast<int>(facet_cursor_.index) + delta;
    const int count = static_cast<int>(facet_counts(facet_cursor_.group).size());
    if (index >= count) {
        group_index = (group_index + 1) % static_cast<int>(groups.size());
        facet_cursor_.group = groups[static_cast<std::size_t>(group_index)];
        facet_cursor_.index = 0;
        return;
    }
    if (index < 0) {
        group_index =
            (group_index - 1 + static_cast<int>(groups.size())) % static_cast<int>(groups.size());
        facet_cursor_.group = groups[static_cast<std::size_t>(group_index)];
        const int last = static_cast<int>(facet_counts(facet_cursor_.group).size());
        facet_cursor_.index = last > 0 ? static_cast<std::size_t>(last - 1) : 0;
        return;
    }
    facet_cursor_.index = static_cast<std::size_t>(index);
}

void SkillsBrowseCore::toggle_facet_at_cursor() {
    const std::string value = facet_cursor_value();
    if (value.empty())
        return;
    toggle_facet(facet_cursor_.group, value);
    const std::size_t count = facet_counts(facet_cursor_.group).size();
    if (facet_cursor_.index >= count && count > 0)
        facet_cursor_.index = count - 1;
}

std::string SkillsBrowseCore::filter_banner() const {
    if (!query_live_)
        return {};
    return any_facet() ? "filters apply to the loaded results, not the registry"
                       : "results come from a live query";
}

BatchPlan plan_batch(const std::vector<BatchCandidate>& candidates, bool review_required) {
    BatchPlan plan;
    for (const auto& candidate : candidates) {
        if (candidate.exists_in_scope) {
            plan.skipped.push_back(candidate.name + " (name collision)");
            continue;
        }
        if (review_required && !candidate.official && !candidate.previewed) {
            plan.skipped.push_back(candidate.name + " (review required: p reads it)");
            continue;
        }
        plan.install_ids.push_back(candidate.id);
    }
    return plan;
}

std::string batch_report(int installed, const std::vector<std::string>& skipped, int failed,
                         const std::vector<std::string>& failures, bool cancelled) {
    std::string summary = std::to_string(installed) + " installed";
    if (!skipped.empty())
        summary += ", " + std::to_string(skipped.size()) + " skipped";
    if (failed > 0)
        summary += ", " + std::to_string(failed) + " failed";
    std::vector<std::string> details = skipped;
    details.insert(details.end(), failures.begin(), failures.end());
    if (!details.empty()) {
        summary += ": ";
        for (std::size_t i = 0; i < details.size(); ++i) {
            if (i > 0)
                summary += ", ";
            summary += details[i];
        }
    }
    if (cancelled)
        summary += " · cancelled, the rest was not started";
    return summary;
}

bool skill_tab_available(SkillTab tab) noexcept {
    return tab == SkillTab::Top || tab == SkillTab::Installed || tab == SkillTab::Updates;
}

const char* skill_tab_name(SkillTab tab) noexcept {
    switch (tab) {
    case SkillTab::ForYou:
        return "For you";
    case SkillTab::Trending:
        return "Trending";
    case SkillTab::Top:
        return "Top";
    case SkillTab::Official:
        return "Official";
    case SkillTab::Collections:
        return "Collections";
    case SkillTab::Installed:
        return "Installed";
    case SkillTab::Updates:
        return "Updates";
    }
    return "?";
}

std::vector<SkillRow> SkillsBrowseCore::visible() const {
    const bool installed_shelf = tab_ == SkillTab::Installed || tab_ == SkillTab::Updates;
    const auto& source = installed_shelf ? installed_ : rows_;
    std::vector<SkillRow> out;
    out.reserve(source.size());
    for (const auto& row : source) {
        if (tab_ == SkillTab::Updates && !row.update)
            continue;
        if (matches(row))
            out.push_back(row);
    }
    if (sort_ == SkillSort::Installs) {
        std::stable_sort(out.begin(), out.end(), [](const SkillRow& a, const SkillRow& b) {
            if (a.ref.installs != b.ref.installs)
                return a.ref.installs > b.ref.installs;
            return a.ref.name < b.ref.name;
        });
    } else if (sort_ == SkillSort::Name) {
        std::stable_sort(out.begin(), out.end(), [](const SkillRow& a, const SkillRow& b) {
            return a.ref.name < b.ref.name;
        });
    }
    return out;
}

std::size_t SkillsBrowseCore::visible_count() const {
    return visible().size();
}

std::vector<std::pair<std::string, int>> SkillsBrowseCore::categories() const {
    std::map<std::string, int> counts;
    for (const auto& row : visible()) {
        if (!row.category.empty())
            ++counts[row.category];
    }
    std::vector<std::pair<std::string, int>> out(counts.begin(), counts.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    });
    return out;
}

std::optional<SkillRow> SkillsBrowseCore::current() const {
    const auto rows = visible();
    if (selection_ < 0 || static_cast<size_t>(selection_) >= rows.size())
        return std::nullopt;
    return rows[static_cast<size_t>(selection_)];
}

void SkillsBrowseCore::reset_cursor() {
    selection_ = 0;
    scroll_ = 0;
    detail_scroll_ = 0;
}

void SkillsBrowseCore::clamp_selection(int visible_rows) {
    const int count = static_cast<int>(visible_count());
    if (count == 0) {
        selection_ = 0;
        scroll_ = 0;
        return;
    }
    selection_ = std::clamp(selection_, 0, count - 1);
    if (visible_rows <= 0) {
        scroll_ = 0;
        return;
    }
    if (selection_ < scroll_)
        scroll_ = selection_;
    if (selection_ >= scroll_ + visible_rows)
        scroll_ = selection_ - visible_rows + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, count - 1));
}

void SkillsBrowseCore::move(int delta, int visible_rows) {
    selection_ += delta;
    clamp_selection(visible_rows);
}

SkillIntent SkillsBrowseCore::key(int ch, int visible_rows) {
    if (pane_ == SkillPane::Search)
        return plain_key(ch, visible_rows);
    if (pane_ == SkillPane::Facets)
        return facet_key(ch);
    if (auto nav = navigation_key(ch, visible_rows))
        return *nav;
    if (auto global = global_key(ch))
        return *global;
    return action_key(ch);
}

SkillIntent SkillsBrowseCore::facet_key(int ch) {
    switch (ch) {
    case 27: // Esc
    case 'f':
        pane_ = SkillPane::List;
        return {};
    case 'q':
        return intent(SkillIntent::Kind::Close);
    case 'j':
    case keys::kDown:
        move_facet_cursor(1);
        return {};
    case 'k':
    case keys::kUp:
        move_facet_cursor(-1);
        return {};
    case '\r':
    case '\n':
    case ' ':
        toggle_facet_at_cursor();
        return intent(SkillIntent::Kind::ToggleFacet);
    case 'r':
        clear_facets();
        return intent(SkillIntent::Kind::ClearFacets);
    default:
        return {};
    }
}

std::optional<SkillIntent> SkillsBrowseCore::navigation_key(int ch, int visible_rows) {
    if (auto i = vertical_key(ch, visible_rows))
        return i;
    if (auto i = jump_key(ch, visible_rows))
        return i;
    return page_key(ch, visible_rows);
}

std::optional<SkillIntent> SkillsBrowseCore::vertical_key(int ch, int visible_rows) {
    const bool detail = pane_ == SkillPane::Detail;
    if (ch == 'j' || ch == keys::kDown) {
        if (detail)
            scroll_detail(1);
        else
            move(1, visible_rows);
        return SkillIntent{};
    }
    if (ch == 'k' || ch == keys::kUp) {
        if (detail)
            scroll_detail(-1);
        else
            move(-1, visible_rows);
        return SkillIntent{};
    }
    return std::nullopt;
}

std::optional<SkillIntent> SkillsBrowseCore::jump_key(int ch, int visible_rows) {
    if (ch == 'g' || ch == keys::kHome) {
        if (pane_ != SkillPane::Detail) {
            selection_ = 0;
            clamp_selection(visible_rows);
        }
        return SkillIntent{};
    }
    if (ch == 'G' || ch == keys::kEnd) {
        if (pane_ != SkillPane::Detail) {
            selection_ = static_cast<int>(visible_count()) - 1;
            clamp_selection(visible_rows);
        }
        return SkillIntent{};
    }
    return std::nullopt;
}

std::optional<SkillIntent> SkillsBrowseCore::page_key(int ch, int visible_rows) {
    const int page = visible_rows > 0 ? visible_rows : 10;
    switch (ch) {
    case keys::kNPage:
        move(page, page);
        return SkillIntent{};
    case keys::kPPage:
        move(-page, page);
        return SkillIntent{};
    case 4: // Ctrl-D
        move(page / 2, page);
        return SkillIntent{};
    case 21: // Ctrl-U
        move(-(page / 2), page);
        return SkillIntent{};
    default:
        return std::nullopt;
    }
}

std::optional<SkillIntent> SkillsBrowseCore::view_key(int ch) {
    switch (ch) {
    case 's':
        sort_ = sort_ == SkillSort::Installs ? SkillSort::Name
                : sort_ == SkillSort::Name   ? SkillSort::Relevance
                                             : SkillSort::Installs;
        reset_cursor();
        {
            SkillIntent i = intent(SkillIntent::Kind::Sort);
            i.sort = sort_;
            return i;
        }
    case 'R':
        return intent(SkillIntent::Kind::Refresh);
    case 'f':
        if (!facet_groups().empty()) {
            pane_ = SkillPane::Facets;
            facet_cursor_.index = 0;
        }
        return intent(SkillIntent::Kind::FocusFacets);
    case 'r':
        return intent(SkillIntent::Kind::ClearFacets);
    case '?':
        return intent(SkillIntent::Kind::Help);
    default:
        return std::nullopt;
    }
}

std::optional<SkillIntent> SkillsBrowseCore::global_key(int ch) {
    if (auto view = view_key(ch))
        return view;
    switch (ch) {
    case 27: // Esc
        if (pane_ == SkillPane::Detail) {
            pane_ = SkillPane::List;
            return SkillIntent{};
        }
        return intent(SkillIntent::Kind::Close);
    case '/':
        pane_ = SkillPane::Search;
        return SkillIntent{};
    case '\t':
        pane_ = pane_ == SkillPane::Detail ? SkillPane::List : SkillPane::Detail;
        return SkillIntent{};
    case 'q':
        return intent(SkillIntent::Kind::Close);
    default:
        break;
    }
    return shelf_key(ch);
}

std::optional<SkillIntent> SkillsBrowseCore::shelf_key(int ch) {
    if (ch < '1' || ch > '7')
        return std::nullopt;
    SkillIntent i = intent(SkillIntent::Kind::SetTab);
    i.tab = static_cast<SkillTab>(ch - '1');
    set_tab(i.tab);
    return i;
}

SkillIntent SkillsBrowseCore::action_key(int ch) {
    const auto row = current();
    if (!row)
        return {};
    if (auto special = special_action(ch, *row))
        return *special;
    return table_action(ch, *row);
}

std::optional<SkillIntent> SkillsBrowseCore::special_action(int ch, const SkillRow& row) {
    if (ch == '\n' || ch == '\r' || ch == keys::kEnter) {
        pane_ = SkillPane::Detail;
        detail_scroll_ = 0;
        return with_id(SkillIntent::Kind::OpenDetail, row.ref.id);
    }
    if (ch == 'i') {
        if (!marked_.empty()) {
            SkillIntent i = intent(SkillIntent::Kind::InstallMarked);
            i.ids.assign(marked_.begin(), marked_.end());
            return i;
        }
        return with_id(SkillIntent::Kind::Install, row.ref.id);
    }
    if (ch == ' ') {
        if (marked_.count(row.ref.id) != 0)
            marked_.erase(row.ref.id);
        else
            marked_.insert(row.ref.id);
        return SkillIntent{};
    }
    if (ch == 'a') {
        for (const auto& visible_row : visible())
            marked_.insert(visible_row.ref.id);
        return SkillIntent{};
    }
    return std::nullopt;
}

SkillIntent SkillsBrowseCore::table_action(int ch, const SkillRow& row) {
    struct Action {
        int key;
        SkillIntent::Kind kind;
    };
    static constexpr Action kActions[] = {
        {'u', SkillIntent::Kind::Update}, {'U', SkillIntent::Kind::UpdateAll},
        {'D', SkillIntent::Kind::Remove}, {'x', SkillIntent::Kind::ToggleEnabled},
        {'b', SkillIntent::Kind::Block},  {'A', SkillIntent::Kind::Adopt},
        {'d', SkillIntent::Kind::Diff},   {'p', SkillIntent::Kind::Preview},
        {'v', SkillIntent::Kind::Audit},  {'o', SkillIntent::Kind::SourceUrl},
        {'y', SkillIntent::Kind::Copy},
    };
    for (const auto& action : kActions) {
        if (action.key == ch)
            return with_id(action.kind, row.ref.id);
    }
    return {};
}

bool SkillsBrowseCore::edit_filter(int ch) {
    if (ch == 127 || ch == 8 || ch == keys::kBackspace) {
        if (filter_.empty()) {
            pane_ = SkillPane::List;
            return false;
        }
        filter_.pop_back();
        reset_cursor();
        return true;
    }
    if (!is_printable(ch))
        return false;
    filter_.push_back(static_cast<char>(ch));
    reset_cursor();
    return true;
}

SkillIntent SkillsBrowseCore::plain_key(int ch, int visible_rows) {
    if (ch == 27) {
        if (!filter_.empty()) {
            filter_.clear();
            reset_cursor();
            return intent(SkillIntent::Kind::Search);
        }
        pane_ = SkillPane::List;
        return {};
    }
    if (ch == '\n' || ch == '\r' || ch == keys::kEnter) {
        pane_ = SkillPane::List;
        return {};
    }
    if (edit_filter(ch))
        return intent(SkillIntent::Kind::Search);
    if (ch == keys::kUp) {
        move(-1, visible_rows);
        return {};
    }
    if (ch == keys::kDown) {
        move(1, visible_rows);
        return {};
    }
    return {};
}

} // namespace tui
