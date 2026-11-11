#ifndef AMBER_PLUGINS_CACHE_TEXT_H
#define AMBER_PLUGINS_CACHE_TEXT_H

// The gauge's text surfaces, kept pure so every state is testable without a
// terminal, a clock or a plugin lifecycle. The plugin only supplies a Readout.

#include "cache_reuse.h"

#include <string>

namespace agent::plugins::cache {

std::string percent_text(const Readout& readout);
std::string age_text(long long seconds);

// A measured 0/0 below the model's documented minimum cacheable prefix is
// cold, not a misleading "0%" that looks like caching broke.
bool below_minimum(const Readout& readout);

std::string cold_text(const Readout& readout, const std::string& ttl);
std::string warm_text(const Readout& readout);
std::string reuse_text(const Readout& readout, Metric metric, const std::string& ttl);
std::string state_text(const Readout& readout, Metric metric, const std::string& ttl);

// The bottom-bar segment, empty when the state has no number to show.
std::string bar_text(const Readout& readout);

} // namespace agent::plugins::cache

#endif // AMBER_PLUGINS_CACHE_TEXT_H
