
#include "textutil.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <langinfo.h>


namespace tui::text {

std::optional<int> parse_setting_int(const std::string& v, int min, int max) {
    errno = 0;
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (errno != 0 || end == v.c_str() ||
        end != v.c_str() + v.size())
        return std::nullopt;
    if (n < min || n > max) return std::nullopt;
    return static_cast<int>(n);
}

std::optional<double> parse_setting_double(const std::string& v, double min,
                                           double max) {
    errno = 0;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    if (errno != 0 || end == v.c_str() ||
        end != v.c_str() + v.size())
        return std::nullopt;
    // strtod parses "nan"/"inf" fully with errno 0; every comparison with
    // NaN is false so the range check alone would accept it.
    if (!std::isfinite(d)) return std::nullopt;
    if (d < min || d > max) return std::nullopt;
    return d;
}


namespace {
bool detect_utf8() {
    // UTF-8 is the default: virtually every modern terminal (and the model's
    // output) uses it, so we render Unicode (sparkle, arrows, box gauges, em
    // dash, CJK, emoji) by default. We only fall back to ASCII when the user
    // explicitly opts out for a terminal that mis-renders UTF-8 (e.g. PuTTY with
    // a Latin-1 translation table) via AMBER_ASCII=1.
    //
    // NOTE: We deliberately do NOT gate this on nl_langinfo(CODESET): a failed
    // or partial setlocale() (common when LC_CTYPE is malformed or the C locale
    // is in effect) would otherwise silently drop UTF-8 and smear every
    // non-ASCII glyph into garbage. The opt-out is the only thing that disables
    // Unicode; everything else gets UTF-8.
    const char* off = std::getenv("AMBER_ASCII");
    if (!off) return true;
    char c = off[0];
    return c != '1' && c != 'y' && c != 'Y' && c != 't' && c != 'T';
}
} // namespace

bool glyph::utf8() {
    static const bool v = detect_utf8();
    return v;
}

const char* glyph::arrow()    { return utf8() ? "\u2192" : "->"; }
const char* glyph::middot()   { return utf8() ? "\u00b7" : "-"; }
const char* glyph::emdash()   { return utf8() ? "\u2014" : "-"; }
const char* glyph::up()       { return utf8() ? "\u2191" : "^"; }
const char* glyph::down()     { return utf8() ? "\u2193" : "v"; }
const char* glyph::block_l()  { return utf8() ? "\u2590" : "|"; }
const char* glyph::block_r()  { return utf8() ? "\u258c" : "|"; }
const char* glyph::ellipsis() { return utf8() ? "\u2026" : "..."; }
const char* glyph::check()    { return utf8() ? "\u2713" : "+"; }
const char* glyph::cross()    { return utf8() ? "\u2717" : "x"; }

const char* glyph::spinner_round(int frame) {
    static const char* k[4] = {"\u25d0", "\u25d3", "\u25d1", "\u25d2"};  // ◐◓◑◒
    static const char* a[4] = {"|", "/", "-", "\\"};
    return utf8() ? k[((frame % 4) + 4) % 4] : a[((frame % 4) + 4) % 4];
}

const char* glyph::vbar()        { return utf8() ? "\u2502" : "|"; }
const char* glyph::hbar()        { return utf8() ? "\u2500" : "-"; }
const char* glyph::tee_left()    { return utf8() ? "\u251c" : "+"; }
const char* glyph::tee_right()   { return utf8() ? "\u2524" : "+"; }
const char* glyph::tbl_cross()   { return utf8() ? "\u253c" : "+"; }
const char* glyph::top_left()    { return utf8() ? "\u250c" : "+"; }
const char* glyph::top_right()   { return utf8() ? "\u2510" : "+"; }
const char* glyph::bottom_left() { return utf8() ? "\u2514" : "+"; }
const char* glyph::bottom_right(){ return utf8() ? "\u2518" : "+"; }
const char* glyph::top_tee()     { return utf8() ? "\u252c" : "+"; }
const char* glyph::bottom_tee()  { return utf8() ? "\u2534" : "+"; }

std::string git_prompt(const std::string& project, const std::string& branch,
                       int ins, int del) {
    bool u = glyph::utf8();
    std::string p;
    p += u ? "\u250c " : "+ ";
    p += project;
    p += " ";
    p += branch;
    if (ins > 0 || del > 0) {
        p += " ";
        if (ins > 0) { p += "+"; p += std::to_string(ins); }
        p += "/";
        if (del > 0) { p += "-"; p += std::to_string(del); }
    }
    p += u ? " \u276f " : " > ";
    return p;
}

size_t col_to_byte(const std::string& s, int col) {
    if (col <= 0) return 0;
    size_t byte_pos = 0;
    int cur_col = 0;
    while (byte_pos < s.size()) {
        size_t advance = utf8_len(s, byte_pos);
        if (advance == 0) advance = 1;
        std::string cp = s.substr(byte_pos, advance);
        int w = display_cols(cp);
        if (cur_col + w > col) return byte_pos;
        cur_col += w;
        byte_pos += advance;
    }
    return s.size();
}

std::size_t utf8_len(const std::string& s, std::size_t i) {
    auto c = static_cast<unsigned char>(s[i]);
    std::size_t n = 1;
    if (c >= 0x80) {
        if ((c >> 5) == 0x6) n = 2;
        else if ((c >> 4) == 0xE) n = 3;
        else if ((c >> 3) == 0x1E) n = 4;
    }
    // Validate continuation bytes; treat a truncated sequence as 1 byte.
    for (std::size_t k = 1; k < n; ++k)
        if (i + k >= s.size() ||
            (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
            return 1;
    return n;
}

int display_cols(const std::string& s) {
    int cols = 0;
    std::wstring ws = to_wide(s);
    for (wchar_t wc : ws) {
        int w = wcwidth(wc);
        if (w < 0) w = 1;          // undetermined: assume one column
        cols += w;
    }
    return cols;
}

std::vector<std::string> wrap(const std::string& text, int w) {
    if (w <= 0) w = 80;
    std::vector<std::string> out;
    // Sanitize: expand tabs, drop CR, strip ANSI/control bytes that would
    // otherwise be written raw to the terminal (garbage on screen), while
    // preserving valid multibyte UTF-8 (emoji/CJK) intact.
    std::string src;
    src.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        if (c == '\t') { src += "    "; ++i; continue; }
        if (c == '\n') { src += '\n'; ++i; continue; }
        if (c == 0x1b) {                       // ESC: skip a CSI/simple seq
            ++i;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size() &&
                       (text[i] < '@' || text[i] > '~')) ++i;
                if (i < text.size()) ++i;      // final byte
            } else if (i < text.size()) {
                ++i;
            }
            continue;
        }
        if (c < 0x20 || c == 0x7f) { ++i; continue; }  // other control chars
        std::size_t n = utf8_len(text, i);
        if (n == 1 && c >= 0x80) { src += '?'; ++i; continue; } // bad byte
        src.append(text, i, n);
        i += n;
    }
    std::size_t start = 0;
    while (start <= src.size()) {
        std::size_t nl = src.find('\n', start);
        std::string para = (nl == std::string::npos)
                               ? src.substr(start)
                               : src.substr(start, nl - start);
        // word-wrap this paragraph
        if (para.empty()) {
            out.emplace_back("");
        } else {
            std::size_t p = 0;
            while (p < para.size()) {
                // Walk forward up to `w` display columns, counting whole
                // UTF-8 characters (each counted as one column) so we never
                // slice through a multibyte sequence.
                std::size_t q = p;
                int cols = 0;
                while (q < para.size() && cols < w) {
                    q += utf8_len(para, q);
                    ++cols;
                }
                if (q >= para.size()) {
                    out.push_back(para.substr(p));
                    break;
                }
                // find a space to break on within [p, q]
                std::size_t brk = para.rfind(' ', q);
                if (brk == std::string::npos || brk <= p) {
                    out.push_back(para.substr(p, q - p));  // hard split
                    p = q;
                } else {
                    out.push_back(para.substr(p, brk - p));
                    p = brk + 1;                           // skip the space
                }
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::wstring to_wide(const std::string& s) {
    std::wstring w;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t n = utf8_len(s, i);
        wchar_t cp = 0;
        auto c = static_cast<unsigned char>(s[i]);
        if (n == 1) {
            cp = c;
        } else if (n == 2) {
            cp = (c & 0x1F);
        } else if (n == 3) {
            cp = (c & 0x0F);
        } else {
            cp = (c & 0x07);
        }
        for (std::size_t k = 1; k < n; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        w.push_back(cp);
        i += n;
    }
    return w;
}

} // namespace tui::text

