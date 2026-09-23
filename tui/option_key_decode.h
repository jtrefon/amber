#ifndef AMBER_TUI_OPTION_KEY_DECODE_H
#define AMBER_TUI_OPTION_KEY_DECODE_H

#include <cstdint>

// L1 (pure): decode the macOS Option-as-text form. Terminals with default
// Option settings send the literal Option characters as UTF-8 (Option+1 = '¡'
// U+00A1, ...) instead of an ESC prefix. The I/O that reads the bytes stays in
// the loop; only the decoding is here.
namespace tui::option_key_decode {

// Whether `lead` begins a UTF-8 sequence this decoder handles (2-4 bytes).
bool is_lead(unsigned char lead) noexcept;

// Continuation bytes expected after `lead` (1..3); 0 when `lead` is not a
// handled lead byte.
int continuation_bytes(unsigned char lead) noexcept;

// Assemble the codepoint from a lead byte and its continuation bytes:
// `seq[0]` is the lead, `seq[1..need]` are the continuation bytes.
std::uint32_t codepoint(const unsigned char* seq, int need) noexcept;

} // namespace tui::option_key_decode

#endif // AMBER_TUI_OPTION_KEY_DECODE_H
