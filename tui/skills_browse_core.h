#ifndef AMBER_TUI_SKILLS_BROWSE_CORE_H
#define AMBER_TUI_SKILLS_BROWSE_CORE_H

#include "agent/remote_text.h"
#include "agent/skill_index.h"

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

enum class SkillPane : unsigned char { Search, List, Detail };

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
        Help,
        Close,
    };

    Kind kind = Kind::None;
    std::string id;
    std::vector<std::string> ids;
    SkillTab tab = SkillTab::Top;
    SkillSort sort = SkillSort::Relevance;
};

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
    void clamp_selection(int visible_rows);
    void move(int delta, int visible_rows);
    void reset_cursor();
    SkillIntent plain_key(int ch, int visible_rows);
    std::optional<SkillIntent> navigation_key(int ch, int visible_rows);
    std::optional<SkillIntent> vertical_key(int ch, int visible_rows);
    std::optional<SkillIntent> jump_key(int ch, int visible_rows);
    std::optional<SkillIntent> page_key(int ch, int visible_rows);
    std::optional<SkillIntent> global_key(int ch);
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
