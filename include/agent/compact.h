#ifndef AGENT_COMPACT_H
#define AGENT_COMPACT_H

#include <string>

// One formatter for every number the UI shows.
//
// Readouts grow: a lag of 12 400 ms, a session of 2.4 million input tokens, a
// 131 000-token window. Each one that grew a digit at a time was what pushed
// the status bar's fixed regions out of the terminal, so the rule is enforced
// here once rather than re-learned at every call site:
//
//   under 1000    exact            ("42", "512", "999")
//   1000 and up   k/m/g/t, two significant digits, at most three characters
//                                  ("1.5k", "12k", "130k", "1.2m")
//
// A readout can therefore never widen with magnitude, only with its unit.
// Negative input yields "?" (an unknown reading, not a zero).
namespace agent::compact {

std::string count(long n);

// Milliseconds as a duration: 240ms, 1.5s, 12s, 1.5m, 12m, 1.5h. The same
// two-significant-digit rule per unit.
std::string duration_ms(long ms);

// A percentage, clamped to 0..100. Bounded by definition, so it stays exact
// (including "100"); the only three-digit value it can produce is a full bar.
std::string percent(int p);

// Bytes: 512B, 1KB, 1.5KB, 13KB, 1.2MB, 12GB. Binary units (1024), because
// that is what file sizes elsewhere in amber mean.
std::string bytes(long b);

} // namespace agent::compact

#endif // AGENT_COMPACT_H
