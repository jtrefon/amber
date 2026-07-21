// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

// Markdown -> RichLine renderer backed by the vendored md4c library
// (see third_party/md4c). md4c parses CommonMark (plus GitHub-flavored
// tables/tasklists/strikethrough) and drives the callbacks below, which build
// tui::rich::Line values. This keeps the public md::render() signature stable
// so Canvas / Tui are unaware of which parser is underneath (OCP).

#include "markdown.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "textutil.h"
#include "third_party/md4c/md4c.h"


namespace tui::md {

namespace {

using rich::Line;
using rich::Run;

struct RunStyle {
    int pair = 0;
    bool bold = false, dim = false, italic = false, under = false;
    bool operator==(const RunStyle& o) const {
        return pair == o.pair && bold == o.bold && dim == o.dim &&
               italic == o.italic && under == o.under;
    }
};

struct ListFrame {
    bool ordered;
    unsigned index;
};

struct Ctx {
    const Style* st;
    std::vector<Line>* out;

    std::vector<RunStyle> style_stack;   // active inline style (nested spans)
    Line cur;                            // current block's accumulated line
    bool pending_break = false;          // soft break becomes a space

    int heading_level = 0;               // >0 while inside a heading
    int quote_depth = 0;                 // >0 while inside a blockquote
    std::vector<ListFrame> lists;         // active list nesting

    bool in_code = false;
    std::string code_buf;                // raw fenced/indented code
    std::string code_lang;

    // table accumulation
    bool in_table = false;
    int table_cols = 0;
    std::vector<MD_ALIGN> aligns;
    bool row_is_head = false;
    std::vector<std::string> row_cells;
    std::string cell_buf;                // text of the cell being built
    bool in_cell = false;
};

RunStyle base_style(const Style& st) {
    RunStyle s;
    s.pair = st.text_pair;
    return s;
}

void emit_line(Ctx& c, Line l) { c.out->push_back(std::move(l)); }

// Forward declarations (defined below flush_block).
bool is_separator_line(const Line& l);
void emit_hr(Ctx& c);

// Build the indentation/marker prefix for the block currently being flushed.
std::string block_prefix(Ctx& c, bool& use_quote_pair) {
    std::string p;
    for (size_t i = 0; i < c.lists.size(); ++i) {
        p += "  ";  // 2-space indent per nesting level
    }
    if (!c.lists.empty()) {
        const ListFrame& f = c.lists.back();
        if (f.ordered)
            p += std::to_string(f.index) + ". ";
        else
            p += "• ";  // bullet
    }
    if (c.quote_depth > 0) {
        int indent = c.quote_depth > 0 ? (c.quote_depth - 1) * 2 : 0;
        p = std::string(indent, ' ') + "> " + p;
        use_quote_pair = true;
    }
    return p;
}

// Flush the accumulated inline line as a rendered RichLine, applying the
// appropriate prefix (list marker / quote) and heading treatment.
void flush_block(Ctx& c) {
    if (c.cur.runs.empty()) {
        c.cur = Line{};
        return;
    }
    Line l = std::move(c.cur);
    c.cur = Line{};

    bool quote_pair = false;
    std::string prefix = block_prefix(c, quote_pair);

    if (c.heading_level > 0) {
        Line h;
        for (auto& r : l.runs) {
            r.pair = c.st->heading_pair;
            r.bold = true;
        }
        h.runs.insert(h.runs.end(), l.runs.begin(), l.runs.end());
        if (!prefix.empty()) {
            Run pre;
            pre.pair = quote_pair ? c.st->quote_pair : c.st->text_pair;
            pre.text = prefix;
            h.runs.insert(h.runs.begin(), pre);
        }
        emit_line(c, std::move(h));
        return;
    }

    if (!prefix.empty()) {
        Run pre;
        pre.pair = quote_pair ? c.st->quote_pair : c.st->text_pair;
        pre.dim = quote_pair;
        pre.text = prefix;
        l.runs.insert(l.runs.begin(), pre);
    }

    // A paragraph consisting of a single run of separator glyphs (e.g. a
    // model-emitted "─::::::" or "=====" rule) is a thematic break, not text.
    // Render it as a clean horizontal rule instead of literal artifacts.
    if (is_separator_line(l))
        emit_hr(c);
    else
        emit_line(c, std::move(l));
}

// True when a rendered line is a thematic/rule line rather than prose. Two
// cases the model emits:
//   (a) every glyph is a rule character (ASCII - = _ * : | . # + ~, or any
//       non-ASCII glyph like box-drawing ─━═ or bullets ·•);
//   (b) the line is dominated by a single repeated glyph (>=80% of its code
//       points are the same one, length >= 4) — covers decorative "7777…",
//       "....", "||||", "────" separators LLMs like to draw.
bool is_separator_line(const Line& l) {
    if (l.runs.empty()) return false;
    std::string t;
    for (const auto& r : l.runs) t += r.text;
    if (t.empty()) return false;
    // Count codepoints and the most frequent one.
    std::vector<std::string> cps;
    for (size_t i = 0; i < t.size(); i += text::utf8_len(t, i))
        cps.push_back(t.substr(i, text::utf8_len(t, i)));
    if (cps.size() < 4) return false;
    bool all_rule = true;
    for (const auto& cp : cps) {
        if (cp.size() == 1) {
            auto c = static_cast<unsigned char>(cp[0]);
            bool ascii_sep = (c == '-' || c == '=' || c == '_' || c == '*' ||
                              c == ':' || c == '|' || c == '.' || c == '#' ||
                              c == '+' || c == '~');
            if (!ascii_sep) { all_rule = false; break; }
        } else {
            all_rule = false; break;  // multi-byte glyph: not pure-ASCII rule
        }
    }
    if (all_rule) return true;
    // Dominant repeated glyph?
    std::string dom; int best = 0;
    for (const auto& a : cps) {
        int n = 0;
        for (const auto& b : cps) if (a == b) ++n;
        if (n > best) { best = n; dom = a; }
    }
    return best * 5 >= static_cast<int>(cps.size()) * 4 && !dom.empty();
}

void emit_hr(Ctx& c) {
    Line l;
    l.is_hr = true;
    Run r;
    r.pair = c.st->hr_pair;
    r.text = " ";
    l.runs.push_back(r);
    emit_line(c, std::move(l));
}

// Append text with the given style, honoring inline ANSI SGR sequences that the
// model may emit (mapped onto Run styles rather than stripped).
void append_styled(Ctx& c, const std::string& s, const RunStyle& base) {
    RunStyle cur = base;
    size_t i = 0, n = s.size();
    std::string buf;
    auto flush = [&]() {
        if (!buf.empty()) {
            Run r;
            r.pair = cur.pair;
            r.bold = cur.bold;
            r.dim = cur.dim;
            r.italic = cur.italic;
            r.under = cur.under;
            r.text = buf;
            c.cur.runs.push_back(r);
            buf.clear();
        }
    };
    while (i < n) {
        if (static_cast<unsigned char>(s[i]) == 0x1b && i + 1 < n &&
            s[i + 1] == '[') {
            flush();
            // parse SGR; mutate cur; advance i past the sequence
            size_t j = i + 2;
            std::vector<int> nums;
            std::string num;
            while (j < n) {
                char ch = s[j];
                if (std::isdigit(static_cast<unsigned char>(ch)) || ch == ';') {
                    if (ch == ';') {
                        nums.push_back(num.empty() ? 0 : std::stoi(num));
                        num.clear();
                    } else
                        num += ch;
                    ++j;
                } else if (ch == 'm') {
                    if (!num.empty()) nums.push_back(std::stoi(num));
                    ++j;
                    break;
                } else {
                    ++j;
                    break;
                }
            }
            if (nums.empty()) nums.push_back(0);
            for (int code : nums) {
                switch (code) {
                    case 0: cur = base; break;
                    case 1: cur.bold = true; break;
                    case 2: cur.dim = true; break;
                    case 3: cur.italic = true; break;
                    case 4: cur.under = true; break;
                    case 22: cur.bold = cur.dim = false; break;
                    case 23: cur.italic = false; break;
                    case 24: cur.under = false; break;
                    case 30: cur.pair = c.st->text_pair; break;
                    case 31: cur.pair = P_GAUGE_CRIT; break;
                    case 32: cur.pair = c.st->code_pair; break;
                    case 33: cur.pair = P_MD_CODESTR; break;
                    case 34: cur.pair = P_MD_CODECMT; break;
                    case 35: cur.pair = P_MD_CODEKEY; break;
                    case 36: cur.pair = c.st->quote_pair; break;
                    case 37: case 90: cur.pair = c.st->text_pair; break;
                    case 91: cur.pair = P_GAUGE_CRIT; break;
                    case 92: cur.pair = c.st->code_pair; break;
                    case 93: cur.pair = P_MD_CODESTR; break;
                    case 94: cur.pair = P_MD_CODECMT; break;
                    case 95: cur.pair = P_MD_CODEKEY; break;
                    case 96: cur.pair = c.st->quote_pair; break;
                    case 97: cur.pair = c.st->text_pair; break;
                    default: break;
                }
            }
            i = j;
            continue;
        }
        std::size_t cl = text::utf8_len(s, i);
        buf.append(s, i, cl);
        i += cl;
    }
    flush();
}

int enter_block(MD_BLOCKTYPE type, void* detail, void* ud) {
    Ctx& c = *static_cast<Ctx*>(ud);
    switch (type) {
        case MD_BLOCK_H: {
            auto* d = static_cast<MD_BLOCK_H_DETAIL*>(detail);
            c.heading_level = static_cast<int>(d->level);
            break;
        }
        case MD_BLOCK_QUOTE:
            ++c.quote_depth;
            break;
        case MD_BLOCK_CODE: {
            auto* d = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            c.in_code = true;
            c.code_buf.clear();
            c.code_lang.clear();
            if (d && d->lang.text && d->lang.size > 0)
                c.code_lang.assign(d->lang.text, d->lang.size);
            break;
        }
        case MD_BLOCK_UL: {
            c.lists.push_back({false, 0});
            break;
        }
        case MD_BLOCK_OL: {
            auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
            c.lists.push_back({true, d ? d->start : 1});
            break;
        }
        case MD_BLOCK_LI: {
            // Flush any in-progress item text before starting this one (tight
            // nested lists append sibling text to the same c.cur without an
            // intervening block boundary).
            if (!c.cur.runs.empty()) flush_block(c);
            break;
        }
        case MD_BLOCK_TABLE: {
            auto* d = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
            c.in_table = true;
            c.table_cols = d ? static_cast<int>(d->col_count) : 0;
            c.aligns.assign(c.table_cols, MD_ALIGN_DEFAULT);
            c.row_cells.clear();
            break;
        }
        case MD_BLOCK_TH: {
            c.row_is_head = true;
            c.in_cell = true;
            c.cell_buf.clear();
            auto* d = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
            if (d && c.row_cells.size() < c.aligns.size())
                c.aligns[c.row_cells.size()] = d->align;
            break;
        }
        case MD_BLOCK_TD: {
            c.row_is_head = false;
            c.in_cell = true;
            c.cell_buf.clear();
            auto* d = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
            if (d && c.row_cells.size() < c.aligns.size())
                c.aligns[c.row_cells.size()] = d->align;
            break;
        }
        default:
            break;
    }
    return 0;
}

int leave_block(MD_BLOCKTYPE type, void* detail, void* ud) {
    (void)detail;
    Ctx& c = *static_cast<Ctx*>(ud);
    switch (type) {
        case MD_BLOCK_P:
        case MD_BLOCK_H:
            if (c.in_cell) {
                c.row_cells.push_back(c.cell_buf);
                c.cell_buf.clear();
                c.in_cell = false;
            } else {
                flush_block(c);
            }
            c.heading_level = 0;
            break;
        case MD_BLOCK_QUOTE:
            if (c.quote_depth > 0) --c.quote_depth;
            break;
        case MD_BLOCK_CODE: {
            c.in_code = false;
            // Split the captured source into lines and emit each as a code
            // line (with optional heuristic highlighting).
            std::vector<Line> hl =
                highlight(c.code_buf, c.code_lang, c.st->code_pair);
            for (auto& l : hl) emit_line(c, std::move(l));
            if (c.code_buf.empty()) emit_line(c, Line{});  // blank separator
            c.code_buf.clear();
            c.code_lang.clear();
            break;
        }
        case MD_BLOCK_HR: {
            Line l;
            l.is_hr = true;
            Run r;
            r.pair = c.st->hr_pair;
            r.text = " ";
            l.runs.push_back(r);
            emit_line(c, std::move(l));
            break;
        }
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (!c.lists.empty()) c.lists.pop_back();
            break;
        case MD_BLOCK_LI:
            // md4c emits tight list items as bare text (no inner P), so flush
            // the accumulated item text here. For loose lists the inner P
            // already flushed; c.cur will be empty and flush_block is a no-op.
            if (!c.cur.runs.empty()) flush_block(c);
            // Advance the ordered-list counter for the next sibling item.
            if (!c.lists.empty()) {
                ListFrame& f = c.lists.back();
                if (f.ordered) ++f.index;
            }
            break;
        case MD_BLOCK_TR: {
            // Emit the just-finished row, column-aligned.
            int ncol = std::max(c.table_cols, static_cast<int>(c.row_cells.size()));
            std::vector<int> w(ncol, 0);
            for (int k = 0; k < ncol; ++k) {
                std::string t = (k < static_cast<int>(c.row_cells.size()))
                                    ? c.row_cells[k]
                                    : "";
                w[k] = text::display_cols(t);
            }
            Line l;
            Run sep;
            sep.pair = c.st->table_pair;
            sep.text = "│ ";
            l.runs.push_back(sep);
            bool head = c.row_is_head;
            for (int k = 0; k < ncol; ++k) {
                std::string t = (k < static_cast<int>(c.row_cells.size()))
                                    ? c.row_cells[k]
                                    : "";
                int pad = w[k] - text::display_cols(t);
                Run r;
                r.pair = head ? c.st->table_head_pair : c.st->table_pair;
                r.text = t + std::string(std::max(0, pad), ' ');
                l.runs.push_back(r);
                Run sp;
                sp.pair = c.st->table_pair;
                sp.text = " │ ";
                l.runs.push_back(sp);
            }
            emit_line(c, std::move(l));
            if (head) {
                // separator line under the header
                Line hl;
                Run hr;
                hr.pair = c.st->table_pair;
                hr.text = "├";
                for (int k = 0; k < ncol; ++k) {
                    hr.text += std::string(w[k] + 2, '-');
                    hr.text += (k + 1 < ncol) ? "┼" : "┤";
                }
                hl.runs.push_back(hr);
                emit_line(c, std::move(hl));
            }
            c.row_cells.clear();
            c.row_is_head = false;
            break;
        }
        case MD_BLOCK_TABLE:
            c.in_table = false;
            c.aligns.clear();
            emit_line(c, Line{});  // blank separator after table
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            // Cell content is emitted as inline text (no inner paragraph
            // block), so flush the captured cell text here.
            if (c.in_cell || !c.cell_buf.empty()) {
                c.row_cells.push_back(c.cell_buf);
                c.cell_buf.clear();
                c.in_cell = false;
            }
            break;
        default:
            break;
    }
    return 0;
}

int enter_span(MD_SPANTYPE type, void* detail, void* ud) {
    (void)detail;
    Ctx& c = *static_cast<Ctx*>(ud);
    RunStyle s = c.style_stack.empty() ? base_style(*c.st) : c.style_stack.back();
    switch (type) {
        case MD_SPAN_EM: s.italic = true; break;
        case MD_SPAN_STRONG: s.bold = true; break;
        case MD_SPAN_CODE: s.pair = c.st->code_pair; break;
        case MD_SPAN_DEL: s.dim = true; break;  // no strike attr in ncurses
        case MD_SPAN_A: s.pair = c.st->link_pair; s.under = true; break;
        case MD_SPAN_IMG: s.pair = c.st->link_pair; break;
        default: break;
    }
    c.style_stack.push_back(s);
    return 0;
}

int leave_span(MD_SPANTYPE type, void* detail, void* ud) {
    Ctx& c = *static_cast<Ctx*>(ud);
    if (!c.style_stack.empty()) c.style_stack.pop_back();
    (void)type;
    (void)detail;
    return 0;
}

int text_cb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
    Ctx& c = *static_cast<Ctx*>(ud);
    std::string s(text, size);
    RunStyle base = c.style_stack.empty() ? base_style(*c.st) : c.style_stack.back();

    if (c.in_code) {
        c.code_buf += s;
        return 0;
    }
    if (c.in_cell) {
        c.cell_buf += s;
        return 0;
    }

    switch (type) {
        case MD_TEXT_SOFTBR:
            if (c.quote_depth > 0) {
                // Inside a blockquote, render each source line as its own
                // quoted line so the '>' prefix repeats per visual row.
                flush_block(c);
            } else if (!c.cur.runs.empty()) {
                Run r;
                r.pair = base.pair;
                r.text = " ";
                c.cur.runs.push_back(r);
            }
            break;
        case MD_TEXT_BR:
            // Hard break: end the current line, start a new one.
            flush_block(c);
            break;
        default:
            append_styled(c, s, base);
            break;
    }
    return 0;
}

// Heuristically repair the kind of near-markdown LLMs emit: GFM tables that
// have no blank line before them (md4c requires one) and stray separator runs
// (model-emitted "─------─" rules) glued onto a paragraph. Returns markdown
// that md4c can parse into proper tables / horizontal rules.
std::string normalize_markdown(const std::string& md) {
    std::vector<std::string> in, out;
    std::string line;
    for (char i : md) {
        if (i == '\n') { in.push_back(line); line.clear(); }
        else line += i;
    }
    if (!line.empty()) in.push_back(line);

    auto is_blank = [](const std::string& s) {
        return std::all_of(s.begin(), s.end(),
                           [](unsigned char c) { return std::isspace(c); });
    };
    auto is_heading = [](const std::string& s) {
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '#')) ++i;
        return i > 0 && i < s.size() && s[i] != '#';
    };
    // A table row: starts (modulo indent) with '|' and has a second '|'.
    auto is_table_row = [](const std::string& s) {
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size() || s[i] != '|') return false;
        return s.find('|', i + 1) != std::string::npos;
    };
    // A long run of rule glyphs (- ─ ━ ═) glued onto a paragraph (a model that
    // emits ");─------─" instead of a blank line + rule). Splits it onto its own
    // line so is_separator_line() turns it into a clean horizontal rule. Other
    // decorative runs (7777, ...., ||||) are handled standalone by
    // is_separator_line(); only genuine rule-glyph runs are split here.
    auto split_sep_runs = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> parts;
        std::string text, run;
        auto is_sep = [](char c) {
            auto u = static_cast<unsigned char>(c);
            if (u >= 0x80) return true;  // box-drawing / bullets
            return c == '-' || c == '=';
        };
        bool broke = false;
        for (char c : s) {
            if (is_sep(c)) {
                run += c;
            } else {
                if (run.size() >= 12) {
                    if (!text.empty()) parts.push_back(text);
                    parts.push_back(run);
                    text.clear(); broke = true;
                } else if (!run.empty()) {
                    text += run;
                }
                run.clear();
                text += c;
            }
        }
        if (run.size() >= 12) {
            if (!text.empty()) { parts.push_back(text); text.clear(); }
            parts.push_back(run);
            broke = true;
        } else if (!run.empty()) {
            text += run;
        }
        if (!broke) return {s};
        if (!text.empty()) parts.push_back(text);
        return parts;
    };

    for (size_t i = 0; i < in.size(); ++i) {
        std::string l = in[i];
        // Break a long embedded separator run onto its own line so the existing
        // is_separator_line() turns it into a clean horizontal rule.
        auto parts = split_sep_runs(l);
        if (parts.size() > 1) {
            auto all_rule = [](const std::string& s) {
                if (s.empty()) return false;
                return std::all_of(
                    s.begin(), s.end(), [](unsigned char c) {
                        if (c >= 0x80) return true;  // box-drawing / bullets
                        return c == '-' || c == '=';
                    });
            };
            for (auto& p : parts) {
                if (all_rule(p)) {
                    out.emplace_back("");  // blank line so it renders as a rule
                    out.push_back(p);
                } else {
                    out.push_back(p);
                }
            }
            continue;
        }
        // Insert a blank line before a table row that directly follows prose,
        // a heading, or another table row (md4c needs the break to detect it).
        if (is_table_row(l) && i > 0) {
            const std::string& prev = in[i - 1];
            if (!is_blank(prev) && !is_heading(prev) && !is_table_row(prev))
                out.emplace_back("");
        }
        // Repair tables the model emits without a delimiter row (|---|).
        // md4c requires the separator to recognize a table; without it the
        // rows collapse into one garbage line. Synthesize a matching
        // delimiter row immediately after this (header) row when the next
        // non-blank row is a data table row and no delimiter sits between
        // them. We only do this at the start of a table block (the previous
        // emitted line is not itself a table/delimiter row) so we don't emit
        // a delimiter before every body row.
        auto is_delimiter = [](const std::string& s) {
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '|')) ++i;
            if (i == 0) return false;
            bool ok = true, saw = false;
            for (size_t j = i; j < s.size(); ++j) {
                char c = s[j];
                if (c == '|' || c == ' ' || c == ':' || c == '-') {
                    saw = true; continue;
                }
                ok = false; break;
            }
            return ok && saw;
        };
        if (is_table_row(l) && i + 1 < in.size()) {
            size_t n = i + 1;
            while (n < in.size() && is_blank(in[n])) ++n;
            if (n < in.size() && is_table_row(in[n]) && !is_delimiter(in[n])) {
                bool prev_is_table = !out.empty() &&
                    (is_table_row(out.back()) || is_delimiter(out.back()));
                if (!prev_is_table) {
                    int cols = 0;
                    for (char p : l)
                        if (p == '|') ++cols;
                    if (l.front() == '|') --cols;
                    if (cols < 1) cols = 1;
                    std::string sep;
                    for (int c = 0; c < cols; ++c) sep += "|---";
                    sep += "|";
                    out.push_back(l);
                    out.push_back(sep);
                    continue;
                }
            }
        }
        out.push_back(l);
    }

    std::string res;
    for (size_t i = 0; i < out.size(); ++i) {
        res += out[i];
        if (i + 1 < out.size()) res += '\n';
    }
    return res;
}

} // namespace

std::vector<Line> render(const std::string& md, const Style& st) {
    std::vector<Line> out;
    Ctx c;
    c.st = &st;
    c.out = &out;
    c.style_stack.push_back(base_style(st));

    MD_PARSER parser;
    std::memset(&parser, 0, sizeof(parser));
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH;
    parser.enter_block = enter_block;
    parser.leave_block = leave_block;
    parser.enter_span = enter_span;
    parser.leave_span = leave_span;
    parser.text = text_cb;

    const std::string norm = normalize_markdown(md);
    md_parse(norm.c_str(), static_cast<MD_SIZE>(norm.size()), &parser, &c);
    return out;
}

} // namespace tui::md

