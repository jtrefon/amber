// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "rich.h"

#include <algorithm>

#include "textutil.h"

namespace tui {
namespace rich {

int cols(const std::string& s) { return text::display_cols(s); }

namespace {

// Split `text` into words and residual whitespace, keeping each token's byte
// range so we can slice the original (preserving UTF-8) rather than rebuild.
// A word is a maximal run of non-space; spaces between words are kept as
// separate tokens so wrapping preserves a single leading space.
std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t') j += text::utf8_len(s, j);
        if (j > i) { out.push_back(s.substr(i, j - i)); i = j; }
        else {  // whitespace run
            size_t k = i;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
            out.push_back(s.substr(i, k - i));
            i = k;
        }
    }
    return out;
}

} // namespace

std::vector<Line> wrap(const Line& in, int width) {
    if (width <= 0) width = 80;
    std::vector<Line> out;

    // Build a flat list of (token, run-index) so each wrapped physical line
    // keeps the correct style for every piece of text.
    struct Piece { std::string text; size_t run; };
    std::vector<Piece> pieces;
    pieces.reserve(in.runs.size() * 2);
    for (size_t ri = 0; ri < in.runs.size(); ++ri) {
        for (auto& t : tokens(in.runs[ri].text)) {
            if (t.empty()) continue;
            // A single token wider than the column: hard-break it so it does
            // not overflow the canvas. Only safe on pure-ASCII text, where a
            // byte boundary == a display column; multi-byte tokens are left
            // intact (overflow is avoided by the terminal, never corrupted).
            bool ascii = std::all_of(t.begin(), t.end(),
                                    [](char c) { return (unsigned char)c < 0x80; });
            if (ascii && cols(t) > width && width > 0) {
                for (size_t o = 0; o < t.size(); o += (size_t)width)
                    pieces.push_back({t.substr(o, (size_t)width), ri});
            } else {
                pieces.push_back({t, ri});
            }
        }
    }

    Line cur;
    int used = 0;
    bool first = true;
    auto merge_tail = [&]() {
        // Merge the just-pushed run into the previous one if style matches,
        // so a wrapped physical line keeps a compact run list.
        size_t n = cur.runs.size();
        if (n < 2) return;
        Run& a = cur.runs[n - 2];
        Run& b = cur.runs[n - 1];
        if (a.pair == b.pair && a.bold == b.bold && a.dim == b.dim &&
            a.italic == b.italic && a.under == b.under) {
            a.text += b.text;
            cur.runs.pop_back();
        }
    };
    auto flush = [&]() {
        cur.is_code = in.is_code;
        cur.is_hr = in.is_hr;
        cur.heading = in.heading;
        out.push_back(std::move(cur));
        cur = Line{};
        used = 0;
        first = true;
    };

    for (auto& p : pieces) {
        int w = cols(p.text);
        bool space = (p.text.find(' ') != std::string::npos) ||
                     (p.text.find('\t') != std::string::npos);
        if (!first && used + (space ? 1 : w) > width) {
            flush();
            if (space) continue;  // drop the leading space at line start
        }
        Run r = in.runs[p.run];
        if (!first && space) {
            // render the inter-word space as part of the preceding run's style
            r.text = " ";
        } else {
            r.text = p.text;
        }
        cur.runs.push_back(r);
        merge_tail();
        used += (first && space) ? 0 : w;
        first = false;
    }
    if (!cur.runs.empty() || out.empty()) flush();
    return out;
}

} // namespace rich
} // namespace tui
