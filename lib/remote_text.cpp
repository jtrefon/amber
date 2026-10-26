#include "agent/remote_text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace agent {

namespace {

struct Decoded {
    uint32_t cp = 0;
    size_t len = 1;
    bool ok = false;
};

size_t sequence_length(unsigned char c) {
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 0;
}

// Overlong forms, surrogates and out-of-range codepoints are invalid, because
// each of them can be used to smuggle a different character past a
// byte-oriented check.
bool valid_codepoint(uint32_t cp, size_t len) {
    const bool overlong =
        (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
    const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    return !overlong && !surrogate && cp <= 0x10FFFF;
}

// Strict UTF-8 decode of the codepoint starting at `i`.
Decoded decode_utf8(const std::string& s, size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80)
        return {c, 1, true};

    const size_t len = sequence_length(c);
    if (len == 0 || i + len > s.size())
        return {};

    uint32_t cp = c & (0xFFu >> (len + 1));
    for (size_t k = 1; k < len; ++k) {
        const auto cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80)
            return {};
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (!valid_codepoint(cp, len))
        return {};
    return {cp, len, true};
}

// C1 controls, bidi overrides/isolates, zero-width characters and the BOM.
// A terminal has no use for them in a skill name, and they are exactly what
// hides text from the person reading it.
bool in_range(uint32_t cp, uint32_t lo, uint32_t hi) {
    return cp >= lo && cp <= hi;
}

bool is_bidi(uint32_t cp) {
    return in_range(cp, 0x202A, 0x202E) || in_range(cp, 0x2066, 0x2069);
}

bool is_zero_width(uint32_t cp) {
    return in_range(cp, 0x200B, 0x200F) || in_range(cp, 0x2060, 0x2064);
}

bool is_hidden(uint32_t cp) {
    return in_range(cp, 0x80, 0x9F) || is_bidi(cp) || is_zero_width(cp) || cp == 0xFEFF;
}

size_t skip_csi(const std::string& s, size_t j) {
    while (j < s.size() && (s[j] < 0x40 || s[j] > 0x7E))
        ++j;
    return j < s.size() ? j + 1 : j;
}

size_t skip_string_escape(const std::string& s, size_t j) {
    while (j < s.size()) {
        if (s[j] == '\x07')
            return j + 1;
        if (s[j] == '\x1B' && j + 1 < s.size() && s[j + 1] == '\\')
            return j + 2;
        ++j;
    }
    return j;
}

bool is_string_kind(char kind) {
    return kind == ']' || kind == 'P' || kind == 'X' || kind == '^' || kind == '_';
}

// ESC starts one of: CSI (until a 0x40-0x7E final byte), OSC/DCS/SOS/PM/APC
// (until BEL or ST), or a two-byte sequence. Returning the index after it is
// what makes "strip the escape" not also strip the text that follows.
size_t skip_escape(const std::string& s, size_t i) {
    const size_t j = i + 1;
    if (j >= s.size())
        return j;

    const char kind = s[j];
    if (kind == '[')
        return skip_csi(s, j + 1);
    if (is_string_kind(kind))
        return skip_string_escape(s, j + 1);
    return std::min(j + 1, s.size());
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool any_contains(const std::string& haystack, std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(), needles.end(), [&haystack](const char* needle) {
        return haystack.find(needle) != std::string::npos;
    });
}

bool has_word(const std::string& s, const char* word) {
    const std::string w(word);
    size_t pos = s.find(w);
    while (pos != std::string::npos) {
        const bool left = pos == 0 || std::isalnum(static_cast<unsigned char>(s[pos - 1])) == 0;
        const size_t end = pos + w.size();
        const bool right = end >= s.size() || std::isalnum(static_cast<unsigned char>(s[end])) == 0;
        if (left && right)
            return true;
        pos = s.find(w, pos + 1);
    }
    return false;
}

bool is_base64_run(const std::string& line) {
    size_t run = 0;
    for (const unsigned char c : line) {
        const bool b64 = std::isalnum(c) != 0 || c == '+' || c == '/' || c == '=';
        run = b64 ? run + 1 : 0;
        if (run >= 80)
            return true;
    }
    return false;
}

std::string truncate_utf8(std::string s, size_t max_bytes) {
    if (s.size() <= max_bytes)
        return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut;
    s.resize(cut);
    return s;
}

void add_signal(std::vector<RiskSignal>& out, const char* kind, int line, const std::string& text) {
    out.push_back({kind, line, truncate_utf8(sanitize_remote_one_line(text), 120)});
}

void scan_line(const std::string& line, int number, std::vector<RiskSignal>& out) {
    const std::string l = lower_ascii(line);
    const bool shell_fence =
        any_contains(l, {"```sh", "```bash", "```zsh", "```shell", "```powershell"});

    if (any_contains(l, {"http://", "https://"}))
        add_signal(out, "network", number, line);
    if (shell_fence || any_contains(l, {"| sh", "| bash", "| zsh"}))
        add_signal(out, "shell", number, line);
    if (has_word(l, "sudo"))
        add_signal(out, "sudo", number, line);
    if (any_contains(l, {"../", "/etc/", "/private/etc/", "~/."}))
        add_signal(out, "path", number, line);
    if (is_base64_run(line))
        add_signal(out, "base64", number, line);
    if (any_contains(l, {"ignore previous instructions", "disregard previous instructions",
                         "ignore all previous", "you are now", "system prompt"}))
        add_signal(out, "injection", number, line);
}

} // namespace

std::string sanitize_remote_text(const std::string& in) {
    std::string out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size();) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c == 0x1B) {
            i = skip_escape(in, i);
            continue;
        }
        if (c < 0x20) {
            if (c == '\n' || c == '\t')
                out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        if (c == 0x7F) {
            ++i;
            continue;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        const Decoded d = decode_utf8(in, i);
        if (!d.ok) {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        if (is_hidden(d.cp)) {
            i += d.len;
            continue;
        }
        out.append(in, i, d.len);
        i += d.len;
    }
    return out;
}

std::string sanitize_remote_one_line(const std::string& in) {
    const std::string s = sanitize_remote_text(in);
    std::string out;
    out.reserve(s.size());

    bool pending_space = false;
    for (const char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            pending_space = true;
            continue;
        }
        if (pending_space && !out.empty())
            out.push_back(' ');
        pending_space = false;
        out.push_back(c);
    }
    return out;
}

std::vector<RiskSignal> scan_skill_body(const std::string& body) {
    std::vector<RiskSignal> out;
    size_t start = 0;
    int number = 1;
    while (start <= body.size()) {
        const size_t end = body.find('\n', start);
        const std::string line =
            body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        scan_line(line, number, out);
        if (end == std::string::npos)
            break;
        start = end + 1;
        ++number;
    }
    return out;
}

TrustTier trust_tier(bool official, bool audited, bool audit_failed, bool known_source,
                     bool blocked) noexcept {
    if (blocked)
        return TrustTier::Blocked;
    if (official)
        return TrustTier::Official;
    if (audited && !audit_failed)
        return TrustTier::Audited;
    if (audit_failed)
        return TrustTier::Unverified;
    if (known_source)
        return TrustTier::Community;
    return TrustTier::Unverified;
}

const char* trust_tier_name(TrustTier tier) noexcept {
    switch (tier) {
    case TrustTier::Official:
        return "official";
    case TrustTier::Audited:
        return "audited";
    case TrustTier::Community:
        return "community";
    case TrustTier::Unverified:
        return "unverified";
    case TrustTier::Blocked:
        return "blocked";
    }
    return "unverified";
}

} // namespace agent
