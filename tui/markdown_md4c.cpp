// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

// Markdown -> RichLine renderer backed by the vendored md4c library
// (see third_party/md4c). md4c parses CommonMark (plus GitHub-flavored
// tables/tasklists/strikethrough) and drives the callbacks below, which build
// tui::rich::Line values. This keeps the public md::render() signature stable
// so Canvas / Tui are unaware of which parser is underneath (OCP).

#include "markdown.h"

#include <cstring>
#include <string>
#include <vector>

#include "textutil.h"
#include "third_party/md4c/md4c.h"

namespace tui {
namespace md {

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
                    case 37: cur.pair = c.st->text_pair; break;
                    case 90: cur.pair = c.st->text_pair; break;
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
        buf += s[i];
        i += text::utf8_len(s, i);
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

    md_parse(md.c_str(), static_cast<MD_SIZE>(md.size()), &parser, &c);
    return out;
}

} // namespace md
} // namespace tui
