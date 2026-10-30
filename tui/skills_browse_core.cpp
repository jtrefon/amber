#include "tui/skills_browse_core.h"
#include "tui/keys.h"

#include "agent/skill_catalog.h"
#include "tui/skills_format.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
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

// Compact counts: 950, 4.2k, 795K, 1.1M. One decimal only while it adds
// information, and never a trailing ".0".
std::string compact_count(long long value) {
    if (value < 1000)
        return std::to_string(value);
    double scaled = 0;
    const char* suffix = "k";
    if (value < 1000000) {
        scaled = static_cast<double>(value) / 1000.0;
    } else {
        scaled = static_cast<double>(value) / 1000000.0;
        suffix = "M";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), scaled < 10 ? "%.1f" : "%.0f", scaled);
    std::string text = buffer;
    if (text.size() > 2 && text.compare(text.size() - 2, 2, ".0") == 0)
        text.resize(text.size() - 2);
    return text + suffix;
}

std::string compact_age(long long age_ms) {
    const long long seconds = std::max<long long>(0, age_ms / 1000);
    if (seconds < 60)
        return std::to_string(seconds) + "s";
    if (seconds < 3600)
        return std::to_string(seconds / 60) + "m";
    if (seconds < 86400)
        return std::to_string(seconds / 3600) + "h";
    return std::to_string(seconds / 86400) + "d";
}

std::string percent_of(int value, int total) {
    if (total <= 0)
        return {};
    const int percent = static_cast<int>(std::lround(100.0 * value / total));
    return std::to_string(percent) + "% of budget";
}

std::string installed_line(const DetailInstall& install) {
    if (!install.installed)
        return "Not installed";
    std::string line = "Installed";
    if (!install.scope.empty())
        line += " · " + install.scope;
    if (!install.state.empty())
        line += " · " + install.state;
    return line;
}

bool is_text_extension(const std::string& ext) {
    static const std::set<std::string> kText = {
        "md",  "markdown", "txt",  "text", "rst",  "json",  "yaml", "yml",       "toml",
        "ini", "cfg",      "conf", "sh",   "bash", "zsh",   "py",   "rb",        "js",
        "mjs", "cjs",      "ts",   "tsx",  "jsx",  "c",     "h",    "cc",        "cpp",
        "hpp", "rs",       "go",   "java", "kt",   "swift", "css",  "scss",      "html",
        "xml", "csv",      "tsv",  "sql",  "lua",  "pl",    "php",  "terraform",
    };
    return kText.count(ext) != 0;
}

std::vector<std::string> evidence_lines(const agent::SkillDetail& detail,
                                        const DetailInstall& install) {
    std::vector<std::string> lines;
    lines.push_back(installed_line(install));
    if (!detail.assets.empty()) {
        long long bytes = 0;
        for (const auto& asset : detail.assets)
            bytes += static_cast<long long>(asset.bytes);
        lines.push_back(std::to_string(detail.assets.size()) + " files · " + human_size(bytes));
    }
    if (!detail.category.empty())
        lines.push_back("Category " + detail.category);
    if (install.update) {
        std::string line = "update available";
        if (!install.update_hash.empty())
            line += " (" + install.update_hash.substr(0, 8) + ")";
        lines.push_back(std::move(line));
    }
    if (!detail.hash.empty())
        lines.push_back("SKILL.md " + detail.hash.substr(0, 12));
    return lines;
}

} // namespace

DetailBlocks detail_blocks(const agent::SkillDetail& detail, const DetailInstall& install,
                           int body_budget_tokens, int risk_count) {
    DetailBlocks blocks;
    blocks.headline = compact_count(detail.ref.installs) + " installs";
    if (!detail.license.empty())
        blocks.headline += " · " + detail.license;
    blocks.evidence = evidence_lines(detail, install);

    blocks.audit = agent::audit_verdict_name(detail.audit);
    if (!detail.audit_note.empty())
        blocks.audit += " · " + detail.audit_note;
    blocks.audit += risk_count == 0 ? " · no shell, network or path signals"
                                    : " · " + std::to_string(risk_count) + " risk signal" +
                                          (risk_count == 1 ? "" : "s");

    if (!detail.body.empty()) {
        const int tokens = agent::estimate_skill_tokens(detail.body);
        blocks.cost = compact_count(tokens) + " token body";
        const std::string share = percent_of(tokens, body_budget_tokens);
        if (!share.empty())
            blocks.cost += " (" + share + ")";
        blocks.cost += " · +1 discovery line";
    }
    return blocks;
}

std::string source_url(const agent::SkillDetail& detail) {
    if (detail.ref.source.find('/') == std::string::npos) {
        return detail.ref.source.empty() ? std::string() : "https://" + detail.ref.source;
    }
    std::string url = "https://github.com/" + detail.ref.source;
    const std::string branch = detail.default_branch.empty() ? "HEAD" : detail.default_branch;
    if (!detail.skill_dir.empty())
        url += "/tree/" + branch + "/" + detail.skill_dir;
    return url;
}

std::string copy_payload(const agent::SkillRef& ref) {
    return "amber-cli skills install " + ref.id;
}

std::string asset_line(const agent::SkillAsset& asset) {
    return asset.path + "  " + human_size(static_cast<long long>(asset.bytes));
}

bool previewable_asset(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return is_text_extension(ext);
}

std::size_t step_previewable(const std::vector<agent::SkillAsset>& assets, std::size_t from,
                             int step) {
    if (assets.empty() || step == 0)
        return from;
    auto index = static_cast<long long>(from);
    while (true) {
        index += step;
        if (index < 0 || index >= static_cast<long long>(assets.size()))
            return from;
        if (previewable_asset(assets[static_cast<std::size_t>(index)].path))
            return static_cast<std::size_t>(index);
    }
}

std::string base64_encode(const std::string& text) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((text.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < text.size()) {
        const unsigned value = (static_cast<unsigned char>(text[i]) << 16) |
                               (static_cast<unsigned char>(text[i + 1]) << 8) |
                               static_cast<unsigned char>(text[i + 2]);
        out += kAlphabet[(value >> 18) & 0x3F];
        out += kAlphabet[(value >> 12) & 0x3F];
        out += kAlphabet[(value >> 6) & 0x3F];
        out += kAlphabet[value & 0x3F];
        i += 3;
    }
    if (i < text.size()) {
        unsigned value = static_cast<unsigned char>(text[i]) << 16;
        if (i + 1 < text.size())
            value |= static_cast<unsigned char>(text[i + 1]) << 8;
        out += kAlphabet[(value >> 18) & 0x3F];
        out += kAlphabet[(value >> 12) & 0x3F];
        out += i + 1 < text.size() ? kAlphabet[(value >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

bool supports_osc8(const std::string& term, const std::string& term_program) noexcept {
    static const std::set<std::string> kPrograms = {"iTerm.app", "WezTerm", "vscode", "Hyper",
                                                    "Tabby",     "ghostty", "rio"};
    if (kPrograms.count(term_program) != 0)
        return true;
    static const char* const kMarkers[] = {"xterm-kitty", "wezterm", "alacritty", "foot",
                                           "contour"};
    return std::any_of(std::begin(kMarkers), std::end(kMarkers), [&term](const char* marker) {
        return term.find(marker) != std::string::npos;
    });
}

std::string osc8_link(const std::string& url, const std::string& text) {
    if (url.empty())
        return text;
    return "\033]8;;" + url + "\033\\" + text + "\033]8;;\033\\";
}

std::string osc52_copy(const std::string& text) {
    // Terminals disagree about the ceiling; a payload this size is far past the
    // point where the escape is more likely to be truncated than honoured.
    if (text.empty() || text.size() > 65536)
        return {};
    return "\033]52;c;" + base64_encode(text) + "\a";
}

const std::vector<SkillKeyDoc>& skill_key_table() {
    static const std::vector<SkillKeyDoc> kKeys = {
        {"/", "focus the search field", "/skills search <query>"},
        {"Enter", "open the selected detail", "/skills show <id>"},
        {"Tab", "move focus: search, list, detail", ""},
        {"h / l", "move focus left / right", ""},
        {"Esc", "unwind one pane, then close", ""},
        {"j / k", "move, or scroll the detail", ""},
        {"Ctrl+d / Ctrl+u", "page down / up", ""},
        {"g / G", "first / last row", ""},
        {"1..7", "For you, Trending, Top, Official, Collections, Installed, Updates", ""},
        {"s", "cycle sort: relevance, installs, name", ""},
        {"f", "facets (a modal on narrow terminals)", ""},
        {"r", "reset facets", ""},
        {"i", "install sheet, or the marked rows", "/skills install <id>"},
        {"Space", "mark / unmark a row", ""},
        {"a", "mark every visible row", ""},
        {"u / U", "review one update / apply all reviewed", "/skills update [<name>|--all]"},
        {"d", "diff installed against the remote", ""},
        {"D", "remove the selected skill", "/skills remove <name>"},
        {"x / b", "disable / block", ""},
        {"A", "adopt an unmanaged directory", ""},
        {"p", "full-body pager (real Markdown, / search, n/N)", ""},
        {"v", "audit readout: risk signals", ""},
        {"[ / ]", "previous / next file preview", ""},
        {"o", "source URL (OSC 8 when supported, printed either way)", ""},
        {"y", "copy the install command (OSC 52, printed either way)", ""},
        {"R", "refresh the index cache", "/skills sources --refresh"},
        {"?", "this help", ""},
    };
    return kKeys;
}

std::string source_state(bool answered, bool cached, long long fetched_at_ms,
                         const agent::IndexFailure& error, long long now_ms) {
    if (!answered)
        return "idle";
    const std::string age = fetched_at_ms > 0 && now_ms > fetched_at_ms
                                ? compact_age(now_ms - fetched_at_ms)
                                : std::string();
    const bool unreachable = error.kind == agent::IndexError::Offline ||
                             error.kind == agent::IndexError::Timeout ||
                             error.kind == agent::IndexError::Cancelled;
    if (cached) {
        const std::string data = age.empty() ? "cached" : "cached " + age;
        return unreachable ? "offline · " + data : data;
    }
    switch (error.kind) {
    case agent::IndexError::None:
        return "live";
    case agent::IndexError::Disabled:
        return "disabled";
    case agent::IndexError::Offline:
    case agent::IndexError::Timeout:
    case agent::IndexError::Cancelled:
        return "offline";
    default:
        return "error";
    }
}

std::string retry_countdown(int retry_after_s, long long error_at_ms, long long now_ms) {
    if (retry_after_s <= 0 || error_at_ms <= 0 || now_ms < error_at_ms)
        return {};
    const long long remaining = (retry_after_s * 1000LL) - (now_ms - error_at_ms);
    if (remaining <= 0)
        return {};
    return "in " + std::to_string((remaining + 999) / 1000) + "s";
}

std::string serialize_ui_state(const SkillsUiState& state) {
    nlohmann::json j;
    j["tab"] = static_cast<int>(state.tab);
    j["sort"] = static_cast<int>(state.sort);
    nlohmann::json facets = nlohmann::json::object();
    for (const auto& [group, values] : state.facets)
        facets[facet_group_name(group)] = values;
    j["facets"] = std::move(facets);
    return j.dump();
}

SkillsUiState parse_ui_state(const std::string& text) {
    SkillsUiState state;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return state;
    const int tab = j.value("tab", static_cast<int>(state.tab));
    const int sort = j.value("sort", static_cast<int>(state.sort));
    if (tab >= 0 && tab <= static_cast<int>(SkillTab::Updates))
        state.tab = static_cast<SkillTab>(tab);
    if (sort >= 0 && sort <= static_cast<int>(SkillSort::Name))
        state.sort = static_cast<SkillSort>(sort);
    const auto facets = j.find("facets");
    if (facets == j.end() || !facets->is_object())
        return state;
    for (FacetGroup group :
         {FacetGroup::Status, FacetGroup::Trust, FacetGroup::Category, FacetGroup::Source}) {
        const auto it = facets->find(facet_group_name(group));
        if (it == facets->end() || !it->is_array())
            continue;
        for (const auto& value : *it) {
            if (value.is_string())
                state.facets[group].insert(value.get<std::string>());
        }
    }
    return state;
}

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

void SkillsBrowseCore::set_rows(std::vector<SkillRow> rows, bool stale, agent::IndexFailure error,
                                bool cached, long long fetched_at_ms) {
    rows_ = std::move(rows);
    stale_ = stale;
    error_ = std::move(error);
    cached_ = cached;
    fetched_at_ms_ = fetched_at_ms;
    answered_ = true;
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
    if (tab == tab_)
        return;
    tab_positions_[tab_] = TabPosition{selection_, scroll_};
    tab_ = tab;
    const auto saved = tab_positions_.find(tab);
    selection_ = saved == tab_positions_.end() ? 0 : saved->second.selection;
    scroll_ = saved == tab_positions_.end() ? 0 : saved->second.scroll;
    if (saved == tab_positions_.end())
        reset_cursor();
}

SkillsBrowseCore::TabPosition SkillsBrowseCore::tab_position(SkillTab tab) const {
    if (tab == tab_)
        return TabPosition{selection_, scroll_};
    const auto it = tab_positions_.find(tab);
    return it == tab_positions_.end() ? TabPosition{} : it->second;
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

SkillsUiState SkillsBrowseCore::ui_state() const {
    SkillsUiState state;
    state.tab = tab_;
    state.sort = sort_;
    state.facets = facets_;
    return state;
}

void SkillsBrowseCore::apply_ui_state(const SkillsUiState& state) {
    tab_ = state.tab;
    sort_ = state.sort;
    facets_ = state.facets;
    facet_cursor_ = FacetCursor{};
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

std::vector<SkillRow> merge_shelf_rows(const std::vector<ShelfPage>& pages, bool& stale,
                                       agent::IndexFailure& error) {
    std::vector<SkillRow> merged;
    std::set<std::string> seen;
    stale = false;
    error = {};
    for (const auto& page : pages) {
        stale = stale || page.stale;
        if (error.kind == agent::IndexError::None && page.error.kind != agent::IndexError::None)
            error = page.error;
        for (const auto& row : page.rows) {
            if (seen.insert(row.ref.id).second)
                merged.push_back(row);
        }
    }
    return merged;
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

void BatchOutcome::note_installed(const std::string& name) {
    pending.erase(name);
    ++installed;
}

void BatchOutcome::note_failed(const std::string& name, const std::string& reason) {
    pending.erase(name);
    failures.push_back(name + " (" + reason + ")");
}

std::string BatchOutcome::summary() const {
    return batch_summary(*this);
}

std::string batch_summary(const BatchOutcome& outcome) {
    return batch_report(outcome.installed, outcome.skipped,
                        static_cast<int>(outcome.failures.size()), outcome.failures,
                        outcome.cancelled);
}

std::string audit_badge(agent::AuditVerdict verdict) {
    switch (verdict) {
    case agent::AuditVerdict::Pass:
        return "\u2713";
    case agent::AuditVerdict::Warn:
        return "!";
    case agent::AuditVerdict::Fail:
        return "\u2717";
    case agent::AuditVerdict::NotAudited:
        break;
    }
    return {};
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
    case 'h':
        if (pane_ == SkillPane::Detail)
            pane_ = SkillPane::List;
        else if (pane_ == SkillPane::List)
            pane_ = SkillPane::Search;
        return SkillIntent{};
    case 'l':
        if (pane_ == SkillPane::Search)
            pane_ = SkillPane::List;
        else if (pane_ == SkillPane::List) {
            pane_ = SkillPane::Detail;
            detail_scroll_ = 0;
        }
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
        {'u', SkillIntent::Kind::Update},   {'U', SkillIntent::Kind::UpdateAll},
        {'D', SkillIntent::Kind::Remove},   {'x', SkillIntent::Kind::ToggleEnabled},
        {'b', SkillIntent::Kind::Block},    {'A', SkillIntent::Kind::Adopt},
        {'d', SkillIntent::Kind::Diff},     {'p', SkillIntent::Kind::Preview},
        {'v', SkillIntent::Kind::Audit},    {'o', SkillIntent::Kind::SourceUrl},
        {'y', SkillIntent::Kind::Copy},     {'[', SkillIntent::Kind::FilePrev},
        {']', SkillIntent::Kind::FileNext},
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
