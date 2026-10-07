#pragma once

#include "tui/provider_tabs.h"

#include <string>
#include <vector>

namespace tui {

// The drawer's tab strip, as text.
//
// Rendering lives apart from the strip's content so the layout rules are
// testable without a terminal: which tab is marked, where the counts land, and
// what happens on a narrow screen are all decided here rather than in a paint
// loop.
struct DrawerTabStrip {
    std::string line;
    // Offset of the character that marks the selected tab, for reverse video.
    // npos when nothing is marked (no tabs to show).
    std::size_t selected_mark_pos = std::string::npos;
};

// Render the strip for `tabs` with `selected` active, fitted into `max_width`
// columns. Returns an empty strip when there is nothing to switch between (one
// tab), so the arrow keys keep their normal meaning instead of being claimed for
// a tab bar with one entry.
DrawerTabStrip make_tab_strip(const std::vector<ProviderTab>& tabs, std::size_t selected,
                              int max_width = 200);

// Derive the tab list from a set of drawer rows, or none at all.
//
// Data-driven, not a hardcoded command path: the /set model feed emits composite
// "provider::model" keys and nothing else does, so "every row names a provider"
// is what identifies the model picker. A namespace whose rows are plain names
// gets no tabs, so Left/Right keep moving the caret there -- which is why this
// does not mention "/set model" anywhere.
//
// `active_provider` leads the provider tabs; an empty active (a provider that
// was deleted, or a cold start) just falls back to row order.
// The tab labels in order, for CommandLine::set_provider_tabs (which stores
// names, not counts -- the counts belong to the strip).
std::vector<std::string> tab_names(const std::vector<ProviderTab>& tabs);

// One model row, with the provider in its own padded column.
//
// The composite key "provider::model" puts the provider at the FRONT of the id,
// which reads as though the model belonged to the first provider on the list --
// and on the All tab there is no first. The row therefore separates them and drops
// the separator, so the provider is a column you can read down rather than a
// prefix you have to parse.
std::string model_row(const std::string& provider, const std::string& id, int context);

std::vector<ProviderTab> make_provider_tabs(const std::vector<std::string>& rows,
                                            const std::string& active_provider,
                                            const std::string& filter);

} // namespace tui
