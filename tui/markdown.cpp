// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "markdown.h"

#include <cctype>
#include <sstream>

#include "textutil.h"

namespace tui {
namespace md {

// Lightweight heuristic source highlighter for fenced code. Returns RichLines
// (one per source line) with comment/string/number/keyword runs colored.
// Not language-perfect; aims for pleasant structure on common languages.
//
// (The Markdown block/inline parsing itself lives in markdown_md4c.cpp, which
// is backed by the vendored md4c library and calls highlight() for code
// blocks.)
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
    "done","esac","select",nullptr};

bool is_kw(const std::string& w) {
    for (int k = 0; kw[k]; ++k)
        if (w == kw[k]) return true;
    return false;
}

void hl_push(std::vector<rich::Run>& runs, const std::string& t, int p, bool b) {
    if (!t.empty()) runs.push_back({t, p, b});
}

std::vector<rich::Run> hl_runs(const std::string& line, int code_pair) {
    std::vector<rich::Run> runs;
    size_t i = 0, n = line.size();
    while (i < n) {
        char c = line[i];
        if (c == '#' || (c == '/' && i + 1 < n && line[i + 1] == '/')) {
            hl_push(runs, line.substr(i), P_MD_CODECMT, false);
            break;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            hl_push(runs, line.substr(i), P_MD_CODECMT, false);
            break;
        }
        if (c == '"' || c == '\'' || c == '`') {
            char q = c;
            size_t j = i + 1;
            while (j < n && line[j] != q) {
                if (line[j] == '\\') j += 2;
                else ++j;
            }
            hl_push(runs, line.substr(i, (j < n ? j - i + 1 : n - i)),
                    P_MD_CODESTR, false);
            i = (j < n ? j + 1 : n);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n &&
             std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            size_t j = i;
            while (j < n &&
                   (std::isalnum(static_cast<unsigned char>(line[j])) ||
                    line[j] == '.' || line[j] == 'x' || line[j] == '_'))
                ++j;
            hl_push(runs, line.substr(i, j - i), P_MD_CODENUM, false);
            i = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n &&
                   (std::isalnum(static_cast<unsigned char>(line[j])) ||
                    line[j] == '_'))
                ++j;
            std::string w = line.substr(i, j - i);
            if (is_kw(w)) hl_push(runs, w, P_MD_CODEKEY, true);
            else hl_push(runs, w, code_pair, false);
            i = j;
            continue;
        }
        runs.push_back({std::string(1, c), code_pair, false});
        ++i;
    }
    if (runs.empty()) runs.push_back({line, code_pair, false});
    return runs;
}
} // namespace

std::vector<rich::Line> highlight(const std::string& code,
                                 const std::string& /*lang*/, int code_pair) {
    std::vector<rich::Line> out;
    std::istringstream iss(code);
    std::string ln;
    while (std::getline(iss, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        rich::Line l;
        l.is_code = true;
        l.runs = hl_runs(ln, code_pair);
        out.push_back(l);
    }
    if (out.empty()) out.push_back(rich::Line{});
    return out;
}

} // namespace md
} // namespace tui
