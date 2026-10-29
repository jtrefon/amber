#ifndef AMBER_TUI_SKILLS_BROWSE_CORE_H
#define AMBER_TUI_SKILLS_BROWSE_CORE_H

#include "agent/remote_text.h"
#include "agent/skill_index.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tui {

// The seven shelves. `ForYou`, `Official` and `Collections` are populated by
// feeds; `Installed` and `Updates` come from the catalog/ledger. The core only
// tracks which one is active so the renderer and the service agree.
enum class SkillTab : unsigned char {
    ForYou,
    Trending,
    Top,
    Official,
    Collections,
    Installed,
    Updates,
};

enum class SkillSort : unsigned char { Relevance, Installs, Name };

enum class SkillPane : unsigned char { Search, List, Detail, Facets };

// Local filters. A facet never reaches the registry: it narrows the rows that
// are already loaded, and the header says so when a live query is also active.
enum class FacetGroup : unsigned char { Status, Trust, Category, Source };

const char* facet_group_name(FacetGroup group) noexcept;

// A row as the browser needs it. `ref` is the registry identity; the flags are
// what the row badges render and what the install sheet reads.
struct SkillRow {
    agent::SkillRef ref;
    std::string description;
    std::string category;
    std::string state; // installed: enabled | disabled | blocked | suppressed
    agent::TrustTier tier = agent::TrustTier::Unverified;
    bool installed = false;
    bool update = false;
    bool unmanaged = false;
    bool audited = false;
    bool official = false;
};

// The value a row contributes to a group. Always non-empty: a row with no
// category is the visible `uncategorised` bucket, never a hidden one.
std::string facet_value(const SkillRow& row, FacetGroup group);

// What a key asks the host to do. The core never performs I/O or network: the
// TUI translates an intent into a service call, and the service answers with a
// new row set.
struct SkillIntent {
    enum class Kind : unsigned char {
        None,
        Search, // `id` is the query; the host debounces it
        OpenDetail,
        Install,
        InstallMarked,
        Update,
        UpdateAll,
        Remove,
        ToggleEnabled,
        Block,
        Adopt,
        Diff,
        Preview,
        Audit,
        SourceUrl,
        Copy,
        Retry,
        Refresh,
        SetTab,
        Sort,
        FocusFacets,
        ClearFacets,
        ToggleFacet,
        Help,
        Close,
    };

    Kind kind = Kind::None;
    std::string id;
    std::vector<std::string> ids;
    SkillTab tab = SkillTab::Top;
    SkillSort sort = SkillSort::Relevance;
};

// One row considered for a batch install, before anything is fetched.
struct BatchCandidate {
    std::string id;
    std::string name;
    bool official = false;
    bool previewed = false;       // the body has been read this session
    bool exists_in_scope = false; // a directory of that name is already there
};

// A batch install is decided before any request: skills whose gate is unmet or
// whose name is taken are skipped by name, never installed blind and never
// silently dropped.
struct BatchPlan {
    std::vector<std::string> install_ids;
    std::vector<std::string> skipped; // "name (reason)"
};

BatchPlan plan_batch(const std::vector<BatchCandidate>& candidates, bool review_required);

// Pane widths for one terminal size. Pure so the three breakpoints are tested
// without a terminal.
struct SkillsLayout {
    int facets_w = 0;
    int list_w = 0;
    int detail_w = 0;
    bool show_facets = false;
    bool show_detail = false;
};

SkillsLayout skills_layout(int cols, int rows);

// Whether a shelf has a data source today. The remaining shelves are shown so
// the UI is honest about them instead of silently mapping them to Top.
bool skill_tab_available(SkillTab tab) noexcept;
const char* skill_tab_name(SkillTab tab) noexcept;

// L1 state machine. Owns the filter, selection, scroll, marks and pane focus;
// every key either changes that state or returns an intent.
class SkillsBrowseCore {
public:
    SkillsBrowseCore();

    // Replace the visible set. `stale` and `error` are status, not rows: the
    // header can say "cached" without the list disappearing.
    void set_rows(std::vector<SkillRow> rows, bool stale = false, agent::IndexFailure error = {});
    // Learn a row's category from its detail so facets can count it without a
    // second fetch.
    void set_row_category(const std::string& id, std::string category);
    void set_installed(std::vector<SkillRow> installed);
    void set_status(std::string status);

    SkillIntent key(int ch, int visible_rows = 20);
    // The facet pane's keys, exposed so a host that renders the filters as a
    // modal (narrow terminals) drives the same state machine as the column.
    SkillIntent facet_key(int ch);

    SkillTab tab() const noexcept { return tab_; }
    SkillSort sort() const noexcept { return sort_; }
    SkillPane pane() const noexcept { return pane_; }
    const std::string& filter() const noexcept { return filter_; }
    const std::string& status() const noexcept { return status_; }
    bool stale() const noexcept { return stale_; }
    const agent::IndexFailure& error() const noexcept { return error_; }
    int selection() const noexcept { return selection_; }
    int scroll_offset() const noexcept { return scroll_; }
    int detail_scroll() const noexcept { return detail_scroll_; }
    // Facets: OR within a group, AND across groups.
    void toggle_facet(FacetGroup group, const std::string& value);
    void clear_facets();
    bool facet_selected(FacetGroup group, const std::string& value) const;
    bool any_facet() const noexcept;
    // Counts over the rows that pass the text filter but not the facets, so a
    // count never moves because of its own selection.
    std::vector<std::pair<std::string, int>> facet_counts(FacetGroup group) const;

    // The facet pane's cursor: a group and one of its ranked values. Every
    // group with values is reachable, selected or not, so a filter can always
    // be undone from the keyboard.
    struct FacetCursor {
        FacetGroup group = FacetGroup::Status;
        std::size_t index = 0;
    };
    FacetCursor facet_cursor() const noexcept { return facet_cursor_; }
    std::vector<FacetGroup> facet_groups() const;
    std::string facet_cursor_value() const;
    // Non-empty when a server query limits the set the facets narrow.
    std::string filter_banner() const;
    void set_query_live(bool live) noexcept { query_live_ = live; }

    bool marked(const std::string& id) const { return marked_.count(id) != 0; }
    const std::set<std::string>& marks() const noexcept { return marked_; }
    std::size_t visible_count() const;

    // Category counts among the visible rows, highest first. Empty categories
    // are skipped; the renderer shows an honest "load a detail" state.
    std::vector<std::pair<std::string, int>> categories() const;

    // The filtered/sorted rows, by value so a caller cannot hold a reference
    // across a set_rows(). `visible()` is the display order.
    std::vector<SkillRow> visible() const;
    std::optional<SkillRow> current() const;

    void set_pane(SkillPane pane);
    void set_tab(SkillTab tab);
    void set_sort(SkillSort sort);
    void set_filter(std::string filter);
    void clear_marks();
    void scroll_detail(int delta);

private:
    bool matches(const SkillRow& row) const;
    bool passes_facets(const SkillRow& row) const;
    // Facets count the rows that pass every OTHER group, so a selection keeps a
    // truthful count and selecting a second value still widens the set.
    bool passes_facets_except(const SkillRow& row, FacetGroup skip) const;
    void clamp_selection(int visible_rows);
    void move(int delta, int visible_rows);
    void reset_cursor();
    SkillIntent plain_key(int ch, int visible_rows);
    std::optional<SkillIntent> navigation_key(int ch, int visible_rows);
    std::optional<SkillIntent> vertical_key(int ch, int visible_rows);
    std::optional<SkillIntent> jump_key(int ch, int visible_rows);
    std::optional<SkillIntent> page_key(int ch, int visible_rows);
    std::optional<SkillIntent> global_key(int ch);
    std::optional<SkillIntent> view_key(int ch);
    std::optional<SkillIntent> shelf_key(int ch);
    SkillIntent action_key(int ch);
    std::optional<SkillIntent> special_action(int ch, const SkillRow& row);
    SkillIntent table_action(int ch, const SkillRow& row);
    bool edit_filter(int ch);

    std::vector<SkillRow> rows_;
    std::vector<SkillRow> installed_;
    std::string filter_;
    std::string status_;
    std::set<std::string> marked_;
    std::map<FacetGroup, std::set<std::string>> facets_;
    FacetCursor facet_cursor_;
    void move_facet_cursor(int delta);
    void toggle_facet_at_cursor();
    bool query_live_ = false;
    SkillTab tab_ = SkillTab::Top;
    SkillSort sort_ = SkillSort::Installs;
    SkillPane pane_ = SkillPane::List;
    agent::IndexFailure error_;
    bool stale_ = false;
    int selection_ = 0;
    int scroll_ = 0;
    int detail_scroll_ = 0;
};

} // namespace tui

#endif // AMBER_TUI_SKILLS_BROWSE_CORE_H
