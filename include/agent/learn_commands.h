
#ifndef AGENT_LEARN_COMMANDS_H
#define AGENT_LEARN_COMMANDS_H

#include <string>
#include <vector>

#include "agent/experience.h"

namespace agent {

// One learned item for the /learn panel (id-carrying row).
struct LearnItem {
    std::string id;
    std::string type;  // "memory" | "skill"
    std::string name;
    int evidence = 0;
    double score = 0.0;  // store score metric (matches the listing order)
    bool promoted = false;
    int turn = 0;  // last_confirm_turn
    std::string trigger;  // learned skills only
};

// Curation surface for the learning subsystem, shared by the TUI (/learn,
// /get learn) and the CLI. Pure functions over the MemoryStore port; the TUI
// handlers are thin glue. A null store means experience is disabled.

// Filtered, score-sorted items. `filter` is "" (all), "memory" / "skill"
// (type), or a tag substring.
std::vector<LearnItem> learn_items(const MemoryStore* store,
                                   const std::string& filter);

// "id · type · name · evidence n · score 0.xx · promoted|- · turn n" lines
// (skills append ` · trigger "…"`). Empty store -> "(no learned items)".
// Null store -> "experience store disabled".
std::vector<std::string> learn_show_lines(const MemoryStore* store,
                                          const std::string& filter);

// Full detail page for one id. `error` is set for unknown ids.
std::vector<std::string> learn_inspect_lines(const MemoryStore* store,
                                             const std::string& id,
                                             std::string& error);

// "memories: n/max · skills: n/max · promoted: n · path: <store>" summary.
std::vector<std::string> learn_summary_lines(const MemoryStore* store,
                                             const ExperienceConfig& cfg);

} // namespace agent

#endif // AGENT_LEARN_COMMANDS_H
