#ifndef AGENT_REMOTE_TEXT_H
#define AGENT_REMOTE_TEXT_H

#include <string>
#include <vector>

namespace agent {

// Text that came from a registry is hostile input. A skill name is rendered in
// a terminal and written to a ledger, so an escape sequence there can clear the
// screen, forge a prompt or hide the rest of a row. Sanitizing happens at the
// adapter boundary, before anything is displayed or persisted.
//
// `\n` and `\t` survive (a skill body is multi-line); every other C0 control,
// C1 control, CSI/OSC/DCS escape, bidi override and zero-width character is
// removed. Invalid UTF-8 becomes U+FFFD so the byte cannot desynchronise the
// terminal's decoder.
std::string sanitize_remote_text(const std::string& in);

// One display line: the same sanitizer, then every newline/tab folded to a
// space and runs of spaces collapsed. Used for names, descriptions and row
// labels, where a line break would let remote text invent a second row.
std::string sanitize_remote_one_line(const std::string& in);

// One advisory signal found in a skill body. Advisory is the whole point: this
// reports what a reader should look at, never a safety verdict.
struct RiskSignal {
    std::string kind; // shell | network | sudo | path | base64 | injection
    int line = 0;     // 1-based
    std::string excerpt;
};

// Scan a skill body and its supporting text. Pure, deterministic, no I/O.
std::vector<RiskSignal> scan_skill_body(const std::string& body);

// What the acquisition UI may claim about a skill. `blocked` is a user
// override and outranks everything; `audit_failed` deliberately cannot be
// `audited` no matter what else is true.
enum class TrustTier : unsigned char { Official, Audited, Community, Unverified, Blocked };

TrustTier trust_tier(bool official, bool audited, bool audit_failed, bool known_source,
                     bool blocked) noexcept;

const char* trust_tier_name(TrustTier tier) noexcept;

} // namespace agent

#endif // AGENT_REMOTE_TEXT_H
