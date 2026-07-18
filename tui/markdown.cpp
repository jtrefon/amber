// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "markdown.h"

#include <cctype>
#include <map>
#include <sstream>

#include "textutil.h"

namespace tui {
namespace md {

namespace {

using rich::Line;
using rich::Run;

// ---- inline: ANSI SGR -> Run style ---------------------------------------
// Parse a leading ANSI SGR sequence (ESC [ ... m) at `i`, return the new index
// and mutate `st` (pair + attrs). Unknown codes are ignored gracefully.
void apply_sgr(const std::string& s, size_t& i, Run& st) {
    // s[i] == ESC, s[i+1] == '['
    size_t j = i + 2;
    std::vector<int> nums;
    std::string cur;
    while (j < s.size()) {
        char c = s[j];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == ';') {
            if (c == ';') { nums.push_back(cur.empty() ? 0 : std::stoi(cur)); cur.clear(); }
            else cur += c;
            ++j;
        } else if (c == 'm') {
            if (!cur.empty()) nums.push_back(std::stoi(cur));
            ++j; break;
        } else { ++j; break; }
    }
    if (nums.empty()) nums.push_back(0);
    for (size_t k = 0; k < nums.size();) {
        int code = nums[k];
        switch (code) {
            case 0: st.pair = P_ASSISTANT; st.bold = st.dim = st.italic = st.under = false; break;
            case 1: st.bold = true; break;
            case 2: st.dim = true; break;
            case 3: st.italic = true; break;
            case 4: st.under = true; break;
            case 22: st.bold = st.dim = false; break;
            case 23: st.italic = false; break;
            case 24: st.under = false; break;
            case 30: st.pair = P_ASSISTANT; break;  // fg black (rare)
            case 31: st.pair = P_GAUGE_CRIT; break; // red
            case 32: st.pair = P_MD_CODE; break;    // green
            case 33: st.pair = P_MD_CODESTR; break; // yellow
            case 34: st.pair = P_MD_CODECMT; break; // blue
            case 35: st.pair = P_MD_CODEKEY; break; // magenta
            case 36: st.pair = P_MD_QUOTE; break;   // cyan
            case 37: st.pair = P_ASSISTANT; break;  // white
            case 90: st.pair = P_ASSISTANT; break;  // bright black
            case 91: st.pair = P_GAUGE_CRIT; break;
            case 92: st.pair = P_MD_CODE; break;
            case 93: st.pair = P_MD_CODESTR; break;
            case 94: st.pair = P_MD_CODECMT; break;
            case 95: st.pair = P_MD_CODEKEY; break;
            case 96: st.pair = P_MD_QUOTE; break;
            case 97: st.pair = P_ASSISTANT; break;
            default: break;
        }
        ++k;
    }
    i = j;
}

// Turn a single inline-text string (which may contain **bold**, *italic*,
// `code`, [link](url) and ANSI SGR) into styled runs.
std::vector<Run> inline_runs(const std::string& src, const Style& st) {
    std::vector<Run> runs;
    Run base; base.pair = st.text_pair;
    Run cur = base;
    auto emit = [&](const std::string& t) {
        if (t.empty()) return;
        Run r = cur; r.text = t; runs.push_back(r);
    };

    size_t i = 0, n = src.size();
    std::string buf;
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(src[i]);
        if (c == 0x1b && i + 1 < n && src[i + 1] == '[') {
            emit(buf); buf.clear();
            apply_sgr(src, i, cur);
            continue;
        }
        // inline code `...`
        if (c == '`') {
            size_t end = src.find('`', i + 1);
            if (end != std::string::npos) {
                emit(buf); buf.clear();
                Run r = base; r.pair = st.code_pair; r.text = src.substr(i + 1, end - i - 1);
                runs.push_back(r);
                i = end + 1; continue;
            }
        }
        // bold **...** or __...__
        if ((c == '*' && i + 1 < n && src[i + 1] == '*') ||
            (c == '_' && i + 1 < n && src[i + 1] == '_')) {
            char d = c; size_t end = src.find(std::string(2, d), i + 2);
            if (end != std::string::npos) {
                emit(buf); buf.clear();
                Run r = cur; r.bold = true; r.pair = st.emph_pair;
                r.text = src.substr(i + 2, end - i - 2);
                runs.push_back(r);
                i = end + 2; continue;
            }
        }
        // italic *...* or _..._
        if (c == '*' || c == '_') {
            char d = c; size_t end = src.find(d, i + 1);
            if (end != std::string::npos && end > i + 1) {
                emit(buf); buf.clear();
                Run r = cur; r.italic = true; r.pair = st.emph_pair;
                r.text = src.substr(i + 1, end - i - 1);
                runs.push_back(r);
                i = end + 1; continue;
            }
        }
        // link [text](url)
        if (c == '[') {
            size_t close = src.find(']', i + 1);
            size_t paren = (close != std::string::npos) ? src.find('(', close + 1) : std::string::npos;
            size_t rparen = (paren != std::string::npos) ? src.find(')', paren + 1) : std::string::npos;
            if (close != std::string::npos && paren == close + 1 && rparen != std::string::npos) {
                emit(buf); buf.clear();
                Run r = cur; r.pair = st.link_pair; r.under = true;
                r.text = src.substr(i + 1, close - i - 1);
                runs.push_back(r);
                i = rparen + 1; continue;
            }
        }
        buf += c;
        i += text::utf8_len(src, i);
    }
    emit(buf);
    if (runs.empty()) { Run r = base; r.text = ""; runs.push_back(r); }
    return runs;
}

Line para_line(const std::string& text, const Style& st) {
    Line l; l.runs = inline_runs(text, st); return l;
}

// ---- block parser ---------------------------------------------------------
enum class Blk { None, Para, Code, Quote, Item, Head, Hr, Table };

struct Parser {
    const Style& st;
    std::vector<Line>& out;
    Parser(const Style& s, std::vector<Line>& o) : st(s), out(o) {}

    void push_para(const std::string& t) {
        if (!t.empty()) out.push_back(para_line(t, st));
    }
    void push_hr() {
        Line l; l.is_hr = true; l.runs.push_back({std::string(1, ' '), st.hr_pair});
        out.push_back(l);
    }
    void push_head(int lvl, const std::string& t) {
        Line l; l.heading = lvl;
        Run r; r.pair = st.heading_pair; r.bold = true;
        std::string s = t;
        size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
        if (a != std::string::npos) s = s.substr(a, b - a + 1);
        r.text = std::string(lvl, '#') + " " + s;
        l.runs.push_back(r); out.push_back(l);
    }
    void push_quote(const std::string& t) {
        Line l; Run r; r.pair = st.quote_pair; r.text = "> " + t;
        l.runs.push_back(r); out.push_back(l);
    }
    void push_item(const std::string& t) {
        Line l; Run r; r.pair = st.text_pair; r.text = "  • " + t;
        l.runs.push_back(r); out.push_back(l);
    }
    static std::vector<std::string> split_row(const std::string& row) {
        std::vector<std::string> cells;
        size_t i = 0, n = row.size();
        if (i < n && row[i] == '|') ++i;               // leading |
        while (i < n) {
            size_t j = i;
            while (j < n && row[j] != '|') ++j;
            std::string c = row.substr(i, j - i);
            // trim spaces
            size_t a = c.find_first_not_of(' ');
            size_t b = c.find_last_not_of(' ');
            c = (a == std::string::npos) ? "" : c.substr(a, b - a + 1);
            cells.push_back(c);
            i = (j < n && row[j] == '|') ? j + 1 : j;
        }
        if (!cells.empty() && cells.back().empty()) cells.pop_back();
        return cells;
    }
    static bool is_sep_row(const std::vector<std::string>& cells) {
        if (cells.empty()) return false;
        for (auto& c : cells) {
            for (char ch : c)
                if (ch != '-' && ch != ':' && ch != ' ') return false;
        }
        return true;
    }
    void push_table(const std::vector<std::string>& rows, const Style& st) {
        if (rows.empty()) return;
        std::vector<std::vector<std::string>> grid;
        size_t ncol = 0;
        for (auto& r : rows) {
            auto c = split_row(r);
            if (is_sep_row(c)) continue;     // markdown divider row: skip
            ncol = std::max(ncol, c.size());
            grid.push_back(c);
        }
        if (grid.empty()) return;
        std::vector<int> w(ncol, 0);
        for (auto& c : grid)
            for (size_t k = 0; k < c.size(); ++k)
                w[k] = std::max(w[k], text::display_cols(c[k]));
        for (size_t ri = 0; ri < grid.size(); ++ri) {
            Line l;
            Run sep; sep.pair = st.table_pair;
            sep.text = "│ "; l.runs.push_back(sep);
            for (size_t k = 0; k < ncol; ++k) {
                std::string txt = (k < grid[ri].size()) ? grid[ri][k] : "";
                int pad = w[k] - text::display_cols(txt);
                Run r; r.pair = (ri == 0) ? st.table_head_pair : st.table_pair;
                r.text = txt + std::string(std::max(0, pad), ' ');
                l.runs.push_back(r);
                Run sp; sp.pair = st.table_pair; sp.text = " │ ";
                l.runs.push_back(sp);
            }
            out.push_back(l);
            if (ri == 0) {  // header separator line
                Line hl;
                Run hr; hr.pair = st.table_pair;
                hr.text = "├";
                for (size_t k = 0; k < ncol; ++k) {
                    hr.text += std::string(w[k] + 2, '-');
                    hr.text += (k + 1 < ncol) ? "┼" : "┤";
                }
                hl.runs.push_back(hr); out.push_back(hl);
            }
        }
    }
};

} // namespace

std::vector<Line> render(const std::string& md, const Style& st) {
    std::vector<Line> out;
    Parser P(st, out);

    std::vector<std::string> lines;
    {
        std::istringstream iss(md);
        std::string ln;
        while (std::getline(iss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(ln);
        }
    }

    size_t i = 0, n = lines.size();
    std::string para;
    auto flush_para = [&]() { if (!para.empty()) { P.push_para(para); para.clear(); } };

    while (i < n) {
        std::string l = lines[i];
        // fenced code
        if (l.rfind("```", 0) == 0) {
            flush_para();
            std::string lang = l.substr(3);
            std::string body;
            ++i;
            while (i < n && lines[i].rfind("```", 0) != 0) { body += lines[i]; body += "\n"; ++i; }
            ++i; // skip closing fence
            auto hl = highlight(body, lang, st.code_pair);
            for (auto& hl_line : hl) out.push_back(hl_line);
            out.push_back(Line{}); // blank separator
            continue;
        }
        // heading (# through ######, must be followed by a space or EOL)
        size_t h = 0;
        while (h < l.size() && l[h] == '#') ++h;
        bool heading_ok = h > 0 && h <= 6 &&
                          (h == l.size() || l[h] == ' ');
        if (heading_ok) {
            flush_para();
            // Text starts after the '#'s and the mandatory space. A bare
            // "###" (no space) yields an empty heading, not an over-run.
            size_t start = (h < l.size() && l[h] == ' ') ? h + 1 : h;
            P.push_head(static_cast<int>(h),
                        l.substr(start, l.size() - start));
            ++i; continue;
        }
        // hr
        if (l == "---" || l == "***" || l == "___") { flush_para(); P.push_hr(); ++i; continue; }
        // blockquote
        if (l.rfind("> ", 0) == 0 || l.rfind(">", 0) == 0) {
            flush_para();
            size_t q = (l.size() > 1 && l[1] == ' ') ? 2 : 1;
            P.push_quote(l.substr(q, l.size() - q));
            ++i; continue;
        }
        // list item
        if (l.rfind("- ", 0) == 0 || l.rfind("* ", 0) == 0 ||
            (l.size() > 2 && std::isdigit(static_cast<unsigned char>(l[0])) &&
             l[1] == '.' && l[2] == ' ')) {
            flush_para();
            size_t sp = (l[0] == '-' || l[0] == '*') ? 2 : 3;
            P.push_item(l.substr(sp, l.size() - sp));
            ++i; continue;
        }
        // table row (contains a pipe). Collect consecutive pipe rows and emit
        // them as a column-aligned table (first row = header).
        if (l.find('|') != std::string::npos) {
            flush_para();
            std::vector<std::string> rows;
            while (i < n && lines[i].find('|') != std::string::npos) {
                rows.push_back(lines[i]);
                ++i;
            }
            P.push_table(rows, st);
            continue;
        }
        // paragraph text
        if (l.empty()) { flush_para(); ++i; continue; }
        para += (para.empty() ? "" : " ") + l;
        ++i;
    }
    flush_para();
    return out;
}

// ---- heuristic highlighter -------------------------------------------------
namespace {

const char* kw[] = {
    "auto","bool","break","case","catch","class","const","continue","default",
    "delete","do","else","enum","explicit","export","extern","false","for",
    "friend","if","inline","namespace","new","noexcept","nullptr","private",
    "protected","public","return","sizeof","static","struct","switch","template",
    "this","throw","true","try","typedef","typename","using","virtual","void",
    "while","constexpr","constinit","requires","co_await","co_yield","await",
    "async","fn","let","mut","pub","use","impl","match","def","lambda","import",
    "from","as","with","yield","print","echo","local","function","then","fi",
    "do","done","esac","select",nullptr};

bool is_kw(const std::string& w) {
    for (int k = 0; kw[k]; ++k) if (w == kw[k]) return true;
    return false;
}

std::vector<Run> hl_runs(const std::string& line, int code_pair) {
    std::vector<Run> runs;
    size_t i = 0, n = line.size();
    auto push = [&](const std::string& t, int p, bool b) {
        if (!t.empty()) runs.push_back({t, p, b});
    };
    while (i < n) {
        char c = line[i];
        // line comment
        if ((c == '#' || (c == '/' && i + 1 < n && line[i + 1] == '/')) ) {
            push(line.substr(i), P_MD_CODECMT, false);
            break;
        }
        // block comment start (single line heuristic)
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            push(line.substr(i), P_MD_CODECMT, false); break;
        }
        // string
        if (c == '"' || c == '\'' || c == '`') {
            char q = c; size_t j = i + 1;
            while (j < n && line[j] != q) { if (line[j] == '\\') j += 2; else ++j; }
            push(line.substr(i, (j < n ? j - i + 1 : n - i)), P_MD_CODESTR, false);
            i = (j < n ? j + 1 : n); continue;
        }
        // number
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(line[i+1])))) {
            size_t j = i; while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j]=='.' || line[j]=='x' || line[j]=='_')) ++j;
            push(line.substr(i, j - i), P_MD_CODENUM, false); i = j; continue;
        }
        // identifier / keyword
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i; while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j]=='_')) ++j;
            std::string w = line.substr(i, j - i);
            if (is_kw(w)) push(w, P_MD_CODEKEY, true);
            else push(w, code_pair, false);
            i = j; continue;
        }
        // default char
        runs.push_back({std::string(1, c), code_pair, false});
        ++i;
    }
    if (runs.empty()) runs.push_back({line, code_pair, false});
    return runs;
}

} // namespace

std::vector<Line> highlight(const std::string& code,
                            const std::string& /*lang*/, int code_pair) {
    std::vector<Line> out;
    std::istringstream iss(code);
    std::string ln;
    while (std::getline(iss, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        Line l; l.is_code = true;
        l.runs = hl_runs(ln, code_pair);
        out.push_back(l);
    }
    if (out.empty()) out.push_back(Line{});
    return out;
}

} // namespace md
} // namespace tui
