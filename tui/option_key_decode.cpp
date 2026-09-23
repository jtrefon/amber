#include "tui/option_key_decode.h"

namespace tui::option_key_decode {

bool is_lead(unsigned char lead) noexcept {
    return lead >= 0xC2 && lead <= 0xF4;
}

int continuation_bytes(unsigned char lead) noexcept {
    if (!is_lead(lead))
        return 0;
    return lead < 0xE0 ? 1 : (lead < 0xF0) ? 2 : 3;
}

std::uint32_t codepoint(const unsigned char* seq, int need) noexcept {
    std::uint32_t cp = seq[0] & ((need == 1) ? 0x1Fu : (need == 2) ? 0x0Fu : 0x07u);
    for (int i = 1; i <= need; ++i)
        cp = (cp << 6u) | (seq[i] & 0x3Fu);
    return cp;
}

} // namespace tui::option_key_decode
