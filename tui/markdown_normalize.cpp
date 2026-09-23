#include "tui/markdown_normalize.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <vector>

namespace tui::markdown_normalize {

namespace {

bool is_blank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

bool is_heading(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '#'))
        ++i;
    return i > 0 && i < s.size() && s[i] != '#';
}

// A table row: starts (modulo indent) with '|' and has a second '|'.
bool is_table_row(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    if (i >= s.size() || s[i] != '|')
        return false;
    return s.find('|', i + 1) != std::string::npos;
}

// A delimiter row (|---|), possibly indented. One definition for both passes —
// they used to carry identical copies.
bool is_delimiter_row(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '|'))
        ++i;
    if (i == 0)
        return false;
    bool ok = true, saw = false;
    for (size_t j = i; j < s.size(); ++j) {
        const char c = s[j];
        if (c == '|' || c == ' ' || c == ':' || c == '-') {
            saw = true;
            continue;
        }
        ok = false;
        break;
    }
    return ok && saw;
}

// A rule glyph: '-' or '=', or any non-ASCII byte (box-drawing / bullets).
bool is_rule_char(char c) {
    if (static_cast<unsigned char>(c) >= 0x80)
        return true;
    return c == '-' || c == '=';
}

bool is_rule_run(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), is_rule_char);
}

std::vector<std::string> split_lines(const std::string& md) {
    std::vector<std::string> in;
    std::string line;
    for (char i : md) {
        if (i == '\n') {
            in.push_back(line);
            line.clear();
        } else {
            line += i;
        }
    }
    if (!line.empty())
        in.push_back(line);
    return in;
}

// A long run of rule glyphs (- ─ ━ ═) glued onto a paragraph (a model that emits
// ");─------─" instead of a blank line + rule). Splits it onto its own line so
// is_separator_line() turns it into a clean horizontal rule. Other decorative
// runs (7777, ...., ||||) are handled standalone by is_separator_line(); only
// genuine rule-glyph runs are split here.
std::vector<std::string> split_sep_runs(const std::string& s) {
    constexpr size_t kMinRun = 12;
    std::vector<std::string> parts;
    std::string text, run;
    bool broke = false;
    for (char c : s) {
        if (is_rule_char(c)) {
            run += c;
            continue;
        }
        if (run.size() >= kMinRun) {
            if (!text.empty())
                parts.push_back(text);
            parts.push_back(run);
            text.clear();
            broke = true;
        } else if (!run.empty()) {
            text += run;
        }
        run.clear();
        text += c;
    }
    if (run.size() >= kMinRun) {
        if (!text.empty()) {
            parts.push_back(text);
            text.clear();
        }
        parts.push_back(run);
        broke = true;
    } else if (!run.empty()) {
        text += run;
    }
    if (!broke)
        return {s};
    if (!text.empty())
        parts.push_back(text);
    return parts;
}

// Pass 1: line-level repairs — separator runs, table spacing, missing delimiters.
std::vector<std::string> repair_lines(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string& l = in[i];
        const std::vector<std::string> parts = split_sep_runs(l);
        if (parts.size() > 1) {
            for (const auto& p : parts) {
                if (is_rule_run(p))
                    out.emplace_back(""); // blank line so it renders as a rule
                out.push_back(p);
            }
            continue;
        }
        // Insert a blank line before a table row that directly follows prose, a
        // heading, or another table row (md4c needs the break to detect it).
        if (is_table_row(l) && i > 0) {
            const std::string& prev = in[i - 1];
            if (!is_blank(prev) && !is_heading(prev) && !is_table_row(prev))
                out.emplace_back("");
        }
        // Repair tables the model emits without a delimiter row (|---|): md4c
        // needs one or the rows collapse into garbage. Only at the start of a
        // table block, so we do not emit a delimiter before every body row.
        if (is_table_row(l) && i + 1 < in.size()) {
            size_t n = i + 1;
            while (n < in.size() && is_blank(in[n]))
                ++n;
            if (n < in.size() && is_table_row(in[n]) && !is_delimiter_row(in[n])) {
                const bool prev_is_table =
                    !out.empty() && (is_table_row(out.back()) || is_delimiter_row(out.back()));
                if (!prev_is_table) {
                    int cols = 0;
                    for (char p : l)
                        if (p == '|')
                            ++cols;
                    if (l.front() == '|')
                        --cols;
                    if (cols < 1)
                        cols = 1;
                    std::string sep;
                    for (int c = 0; c < cols; ++c)
                        sep += "|---";
                    sep += '|';
                    out.push_back(l);
                    out.push_back(sep);
                    continue;
                }
            }
        }
        out.push_back(l);
    }
    return out;
}

// Columns a table row declares, counting unescaped pipes.
int count_cols(const std::string& s) {
    int pipes = 0;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '|' && (i == 0 || s[i - 1] != '\\'))
            ++pipes;
    if (pipes == 0)
        return 0;
    const size_t first = s.find_first_not_of(' ');
    const size_t last = s.find_last_not_of(' ');
    if (first == std::string::npos || last == std::string::npos)
        return 0;
    const bool starts_pipe = s[first] == '|';
    const bool ends_pipe = s[last] == '|';
    if (starts_pipe && ends_pipe)
        return std::max(0, pipes - 1);
    if (starts_pipe || ends_pipe)
        return pipes;
    return pipes + 1;
}

// Pass 2: pad ragged tables where the header has fewer columns than body rows.
// The LLM sometimes emits `| Vuln | Real path |` (2 cols) followed by 5-col body
// rows; md4c then locks the table to 2 cols and the extra cells are lost,
// appearing as raw `| ... |` paragraphs. Expand every row to the block's max.
void pad_ragged_tables(std::vector<std::string>& out) {
    for (size_t i = 0; i < out.size();) {
        if (!is_table_row(out[i]) && !is_delimiter_row(out[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < out.size() && (is_table_row(out[i]) || is_delimiter_row(out[i])))
            ++i;
        const size_t end = i;
        int max_cols = 0;
        for (size_t k = start; k < end; ++k)
            if (!is_blank(out[k]))
                max_cols = std::max(max_cols, count_cols(out[k]));
        if (max_cols <= 0)
            continue;
        for (size_t k = start; k < end; ++k) {
            if (is_blank(out[k]) || count_cols(out[k]) >= max_cols)
                continue;
            const bool delim = is_delimiter_row(out[k]);
            std::string s = out[k];
            const size_t f = s.find_first_not_of(' ');
            const size_t l = s.find_last_not_of(' ');
            if (f == std::string::npos)
                continue;
            s = s.substr(f, l - f + 1);
            if (s.front() != '|')
                s.insert(0, "| ");
            if (s.back() != '|')
                s += " |";
            int cur = count_cols(s);
            while (cur < max_cols) {
                s += delim ? "---|" : " |";
                ++cur;
            }
            out[k] = s;
        }
    }
}

} // namespace

std::string normalize(const std::string& md) {
    std::vector<std::string> out = repair_lines(split_lines(md));
    pad_ragged_tables(out);

    std::string res;
    for (size_t i = 0; i < out.size(); ++i) {
        res += out[i];
        if (i + 1 < out.size())
            res += '\n';
    }
    return res;
}

} // namespace tui::markdown_normalize
