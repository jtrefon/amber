
#include "agent/shell_classify.h"
#include "agent/workspace.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace agent {

namespace fs = std::filesystem;

namespace {

// One shell token plus whether it was quoted (quoted tokens are data, not
// syntax: `echo "a|b"` prints, `echo a|b` pipes).
struct Tok {
    std::string text;
    bool quoted = false;
};

// Consume one char inside a quoted span; true when the span ended.
bool consume_quoted(char c, char quote, std::string& cur) {
    if (c == quote)
        return true;
    cur += c;
    return false;
}

// Push the accumulated token, if any, and start a new one.
void flush_token(std::vector<Tok>& out, std::string& cur, bool& quoted) {
    if (cur.empty())
        return;
    out.push_back({cur, quoted});
    cur.clear();
    quoted = false;
}

// Split a command line honoring single/double quotes. Quoted spans become a
// single token flagged quoted; quote characters are stripped.
std::vector<Tok> tokenize(const std::string& s) {
    std::vector<Tok> out;
    std::string cur;
    bool quoted = false;
    bool in_single = false, in_double = false;
    for (char c : s) {
        if (in_single) {
            in_single = !consume_quoted(c, '\'', cur);
            quoted = true;
            continue;
        }
        if (in_double) {
            in_double = !consume_quoted(c, '"', cur);
            quoted = true;
            continue;
        }
        if (c == '\'') {
            in_single = true;
            continue;
        }
        if (c == '"') {
            in_double = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            flush_token(out, cur, quoted);
            continue;
        }
        cur += c;
    }
    flush_token(out, cur, quoted);
    return out;
}

std::vector<std::string> pattern_words(const std::string& pattern) {
    std::vector<std::string> w;
    for (const auto& t : tokenize(pattern))
        w.push_back(t.text);
    return w;
}

// Heads whose arguments are never executed and never name write targets:
// they only read or print, so path-like arguments (even /etc/...) are data.
// `cd` is deliberately excluded: it changes process state and enables
// relative-path escapes, so it is never read-only.
bool is_reader_head(const std::string& h) {
    static const char* kReaders[] = {"ls",       "cat",  "grep", "head",   "tail",  "wc",
                                     "sort",     "uniq", "diff", "pwd",    "which", "env",
                                     "printenv", "date", "echo", "printf", "export"};
    return std::any_of(std::begin(kReaders), std::end(kReaders),
                       [&](const char* s) { return h == s; });
}

// Exact chain operators: the command continues with a new command after them.
bool is_chain_op(const std::string& t) {
    return t == ";" || t == "&&" || t == "||" || t == "|";
}

// True for a token that escapes to a background job, a substitution, or a
// glued operator (`cmd&`, `a;b`, `x&&y`, `$(...)`, backticks). Quoted tokens
// are data and never count. File-descriptor duplicates (2>&1, 1>&2) are not
// escapes.
bool is_escape_token(const Tok& t) {
    if (t.quoted)
        return false;
    const std::string& s = t.text;
    if (s == "&" || s == "`" || s.rfind("$(", 0) == 0)
        return true;
    if (s.rfind("2>&", 0) == 0 || s.rfind("1>&", 0) == 0)
        return false;
    // Standalone chain operators (; && || |) are proper separators, not
    // escapes — they are split into segments below. Only glued operators
    // (a;b, cmd&, x|y) are escapes.
    if (is_chain_op(s))
        return false;
    return std::any_of(s.begin(), s.end(),
                       [](char c) { return c == ';' || c == '&' || c == '|' || c == '`'; });
}

// Bare redirection operators: the target is the next token.
bool is_bare_output_redirect(const std::string& s) {
    return s == ">" || s == ">>" || s == "&>" || s == "2>" || s == "2>>" || s == "1>" || s == "1>>";
}

// Length of the operator prefix (">", "2>", "1>", "&>"), or npos when the token
// is not an output redirect.
std::size_t redirect_op_length(const std::string& s) {
    if (!s.empty() && s[0] == '>')
        return 1;
    if (s.rfind("2>", 0) == 0 || s.rfind("1>", 0) == 0 || s.rfind("&>", 0) == 0)
        return 2;
    return std::string::npos;
}

// Output redirection (">", ">>", ">f", "2>f", "&>f"): the command writes a
// file. Input redirects ("<", "<f") read; fd dups (2>&1) redirect within the
// process. Returns the target path ("" when the target is the next token).
bool output_redirect_target(const Tok& t, std::string& target) {
    if (t.quoted)
        return false;
    const std::string& s = t.text;
    if (is_bare_output_redirect(s))
        return true; // the target follows
    std::size_t i = redirect_op_length(s);
    if (i == std::string::npos)
        return false;
    while (i < s.size() && s[i] == '>')
        ++i;
    target = s.substr(i);
    return true;
}

bool is_input_redirect(const Tok& t) {
    if (t.quoted)
        return false;
    const std::string& s = t.text;
    if (s == "<")
        return true;
    if (s[0] == '<')
        return true;
    if (s.rfind("0<", 0) == 0)
        return true;
    return false;
}

// Skip leading `VAR=value` assignments; returns the first command token index.
std::size_t first_command_token(const std::vector<Tok>& toks) {
    std::size_t i = 0;
    while (i < toks.size()) {
        const std::string& t = toks[i].text;
        std::size_t eq = t.find('=');
        if (eq == std::string::npos || eq == 0)
            break;
        bool name_ok = true;
        for (std::size_t k = 0; k < eq; ++k)
            if (!isalnum(static_cast<unsigned char>(t[k])) && t[k] != '_') {
                name_ok = false;
                break;
            }
        if (!name_ok)
            break;
        ++i;
    }
    return i;
}

// Lexically normalized containing directory of a path. Deliberately NOT
// canonicalized: /tmp resolves to /private/tmp on macOS through a symlink,
// and the folder scope must be stable for grants and display regardless of
// which alias the user (or model) typed.
std::string containing_dir(const std::string& path) {
    fs::path p = fs::path(path).lexically_normal();
    if (p.has_parent_path() && p.parent_path() != p)
        return p.parent_path().generic_string();
    return p.generic_string();
}

// Flags and variables never name paths.
bool is_flag_or_var(const std::string& s) {
    return s.empty() || s[0] == '-' || s[0] == '$';
}

// A token that names a path rather than a bare word resolved by cwd.
bool is_path_like(const std::string& s) {
    return s.find('/') != std::string::npos || s == "." || s == ".." || s.rfind("~/", 0) == 0 ||
           s.rfind("../", 0) == 0;
}

// If an unquoted token names a path that escapes the workspace root, return
// the folder scope id ("outside:/abs/dir"); otherwise "". The workspace root
// is read from Workspace::root() — the parameter keeps the signature explicit
// for tests and matches the policy engine's calls.
std::string outside_scope_for(const Tok& t, bool is_target) {
    const std::string& s = t.text;
    if (is_flag_or_var(s))
        return "";
    if (t.quoted && !is_target)
        return ""; // quoted data, not a path
    const bool path_like = is_path_like(s);
    if (!is_target && !path_like)
        return ""; // bare word resolved by cwd
    if (is_target && !path_like && !t.quoted)
        return ""; // bare file name
    std::string resolved, err;
    if (Workspace::confine(s, resolved, err))
        return "";
    return "outside:" + containing_dir(s);
}

bool pattern_matches(const std::string& pattern, const std::vector<std::string>& ws,
                     std::size_t start) {
    std::vector<std::string> want_words = pattern_words(pattern);
    if (want_words.empty() || start + want_words.size() > ws.size())
        return false;
    for (std::size_t i = 0; i < want_words.size(); ++i)
        if (ws[start + i] != want_words[i])
            return false;
    return true;
}

// Tokenize a whole command line, inserting a chain-op sentinel between lines so
// an embedded newline separates commands like ";". The tokenizer treats "\n" as
// whitespace, so without the sentinel "echo a\ncat /etc/passwd" would merge into
// one segment attributed to `echo`.
std::vector<Tok> tokenize_command(const std::string& command) {
    std::vector<Tok> toks;
    std::size_t pos = 0;
    bool first_line = true;
    while (pos <= command.size()) {
        std::size_t nl = command.find('\n', pos);
        std::string line =
            (nl == std::string::npos) ? command.substr(pos) : command.substr(pos, nl - pos);
        if (!first_line && !toks.empty())
            toks.push_back({";", false}); // chain-op sentinel
        auto line_toks = tokenize(line);
        toks.insert(toks.end(), line_toks.begin(), line_toks.end());
        first_line = false;
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return toks;
}

bool has_escape_token(const std::vector<Tok>& toks) {
    return std::any_of(toks.begin(), toks.end(), is_escape_token);
}

// Segment boundaries at chain operators. Each segment is a command of its own;
// the whole command is never ReadOnly if it composes.
std::vector<std::pair<std::size_t, std::size_t>> split_segments(const std::vector<Tok>& toks) {
    std::vector<std::pair<std::size_t, std::size_t>> segs;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (is_chain_op(toks[i].text)) {
            segs.emplace_back(begin, i);
            begin = i + 1;
        }
    }
    segs.emplace_back(begin, toks.size());
    return segs;
}

// git subcommands that only read. Everything else (add, commit, push, reset)
// writes or is dangerous.
bool is_git_read_subcommand(const std::string& sub) {
    static const char* kReadSub[] = {"status", "diff",     "log",       "show", "branch",
                                     "remote", "ls-files", "rev-parse", "help"};
    return std::any_of(std::begin(kReadSub), std::end(kReadSub),
                       [&](const char* s) { return sub == s; });
}

// A head that cannot be proven safe: path-qualified, flag-like, or empty.
bool is_unsafe_head(const std::string& head) {
    return head.empty() || head[0] == '-' || head.find('/') != std::string::npos;
}

// Flags that turn an otherwise inert head into a mutator.
bool is_mutating_flag(const std::string& head, const std::string& text) {
    if (head == "find")
        return text == "-delete" || text == "-exec" || text == "-execdir" || text == "-ok";
    if (head == "sed")
        return text.rfind("-i", 0) == 0; // "-i" or "-i.bak"
    return false;
}

// What one segment's path/redirect/flag scan found.
struct SegmentScan {
    bool write = false;  // a redirect or a mutator flag writes
    std::string outside; // first workspace-escaping path, if any
};

// Redirect, input-redirect, mutator-flag and path-escape scan for one segment.
// `head_index` is the absolute token index of the segment's head.
// Record the first out-of-workspace scope seen; a later one does not override
// it, so the first escape is the one reported.
void note_outside(SegmentScan& scan, const Tok& t, bool is_target) {
    if (!scan.outside.empty())
        return;
    const std::string scope = outside_scope_for(t, is_target);
    if (!scope.empty())
        scan.outside = scope;
}

// A redirect writes a file. When the target is on the same token, scan it for
// a path escape; a bare redirect defers to the next token. Returns true when
// the token was a redirect at all.
bool note_redirect(SegmentScan& scan, const Tok& t, bool& target_next) {
    std::string rtarget;
    if (!output_redirect_target(t, rtarget))
        return false;
    scan.write = true; // a redirect writes a file
    if (rtarget.empty()) {
        target_next = true;
    } else {
        const Tok target{rtarget, false};
        note_outside(scan, target, /*is_target=*/true);
    }
    return true;
}

SegmentScan scan_segment(const std::vector<Tok>& toks, std::size_t begin, std::size_t end,
                         std::size_t head_index, const std::string& head) {
    SegmentScan scan;
    bool target_next = false; // the previous token was a bare redirect
    for (std::size_t i = begin; i < end; ++i) {
        const Tok& t = toks[i];
        if (target_next) {
            note_outside(scan, t, /*is_target=*/true);
            target_next = false;
            continue;
        }
        if (note_redirect(scan, t, target_next))
            continue;
        if (is_input_redirect(t)) {
            // The input target is the next token; scan it for a path escape
            // (cat < /etc/passwd must be Outside).
            target_next = true;
            continue;
        }
        if (i == head_index)
            continue; // the head itself
        if (is_mutating_flag(head, t.text)) {
            scan.write = true;
            continue;
        }
        // Every head, readers included, gets a path-escape check on its
        // arguments: a reader still skips the write classification below, but
        // an out-of-workspace path is caught here.
        note_outside(scan, t, /*is_target=*/false);
    }
    return scan;
}

// The destructive pattern matching at this segment's head, or "".
std::string match_destructive_pattern(const std::vector<std::string>& words, std::size_t offset) {
    for (const auto& pat : destructive_command_patterns())
        if (pattern_matches(pat, words, offset))
            return pat;
    return "";
}

// What one segment contributes to the whole command's verdict.
struct SegmentVerdict {
    bool unsafe_head = false; // path-qualified/unknown binary: fail safe
    bool writes = false;
    std::string scope;       // "bash:git add" — the grant a write earns
    std::string outside;     // first workspace-escaping path in this segment
    std::string destructive; // destructive pattern matched at the head
};

// True when the segment's head is `git` followed by a read-only subcommand.
bool is_git_read_segment(const std::vector<Tok>& toks, std::size_t h, std::size_t end,
                         const std::string& head) {
    return head == "git" && h + 1 < end && is_git_read_subcommand(toks[h + 1].text);
}

// The grant a write-capable head earns when nothing else marked the segment a
// write. "" means the head is a reader, which never earns one; a git writer is
// scoped to its subcommand, everything else to its head.
std::string write_scope_for_head(const std::vector<Tok>& toks, std::size_t h, std::size_t end,
                                 const std::string& head, bool reader) {
    if (reader)
        return "";
    if (head == "git" && h + 1 < end)
        return "bash:git " + toks[h + 1].text;
    return "bash:" + head;
}

// Classify one segment. `outside_so_far` is the whole command's first escaping
// path *before* this segment, which the write-head rule consults.
SegmentVerdict classify_segment(const std::vector<Tok>& toks,
                                const std::pair<std::size_t, std::size_t>& seg,
                                const std::string& outside_so_far) {
    SegmentVerdict verdict;
    std::size_t head_i = first_command_token(std::vector<Tok>(
        toks.begin() + static_cast<long>(seg.first), toks.begin() + static_cast<long>(seg.second)));
    std::size_t h = seg.first + head_i;
    if (h >= seg.second)
        return verdict; // empty segment (e.g. trailing ";")

    const std::string& head = toks[h].text;
    if (is_unsafe_head(head)) {
        verdict.unsafe_head = true;
        return verdict;
    }

    bool git_reader = is_git_read_segment(toks, h, seg.second, head);
    bool reader = is_reader_head(head) || git_reader;

    std::vector<std::string> words;
    for (std::size_t i = seg.first; i < seg.second; ++i)
        words.push_back(toks[i].text);

    SegmentScan scan = scan_segment(toks, seg.first, seg.second, h, head);
    verdict.outside = scan.outside;
    verdict.writes = scan.write;
    verdict.destructive = match_destructive_pattern(words, h - seg.first);

    // A write-capable head with no flag/redirect/path is still a write only
    // when it is a known mutator or unknown; readers are handled above. The
    // rule consults the escaping path *including* this segment's own, as the
    // original did.
    const std::string& outside = scan.outside.empty() ? outside_so_far : scan.outside;
    if (!reader && (verdict.destructive.empty() || outside.empty()) && !verdict.writes) {
        verdict.scope = write_scope_for_head(toks, h, seg.second, head, reader);
        verdict.writes = true;
    }
    if (verdict.writes && verdict.scope.empty())
        verdict.scope = "bash:" + head; // a redirect/flag write is scoped to the head
    return verdict;
}

// The first outside path, destructive pattern and write scope across the
// segments. The whole command's verdict is read off these.
struct Accumulator {
    std::string outside;
    std::string destructive;
    std::string write;
    bool any_write = false;

    void merge(const SegmentVerdict& v) {
        if (!v.outside.empty() && outside.empty())
            outside = v.outside;
        if (!outside.empty())
            return; // outside wins; keep scanning
        if (!v.destructive.empty()) {
            if (destructive.empty())
                destructive = "bash:" + v.destructive;
            return;
        }
        if (v.writes) {
            any_write = true;
            if (write.empty())
                write = v.scope;
        }
    }

    ShellClass verdict(bool composed) const {
        if (!outside.empty())
            return {ShellEffect::Outside, outside};
        if (!destructive.empty())
            return {ShellEffect::Destructive, destructive};
        if (composed || any_write)
            // Composed commands (pipes/sequences of readers) are not provably
            // read-only; benign in-workspace writes run free in WRITE mode.
            return {ShellEffect::Write, write.empty() ? "bash:*" : write};
        return {ShellEffect::ReadOnly, ""};
    }
};

} // namespace

namespace {

// Curated destructive / system-wide command list. These ALWAYS prompt in
// WRITE mode (subject to stored allow/deny rules). The list is the seed for
// the JSON policy store and is user-editable there. Benign commands like
// `cp`/`mv`/`touch` are deliberately absent — they are confined to the
// workspace and run free unless their arguments escape it.
const std::vector<std::string> kDestructivePatterns = {
    "rm",
    "rmdir",
    "dd",
    "mkfs",
    "fdisk",
    "parted",
    "shred",
    "chmod -R",
    "chown -R",
    "sudo",
    "apt",
    "apt-get",
    "dnf",
    "yum",
    "pacman",
    "docker",
    "podman",
    "systemctl",
    "service",
    "shutdown",
    "reboot",
    "halt",
    "poweroff",
    "kill",
    "pkill",
    "killall",
    "git reset",
    "git clean",
    "git push --force",
    "git push -f",
    "git push",
    "git revert",
    "git checkout --",
    "git merge --abort",
    "pip install",
    "npm install",
    "npm publish",
    "yarn add",
    "cargo install",
};

} // namespace

const std::vector<std::string>& destructive_command_patterns() {
    return kDestructivePatterns;
}

// Classify a command line by splitting it into segments and reading the
// verdict off what the segments contributed. Every step is a named helper:
// tokenize, escape scan, segment split, per-segment scan, accumulation.
ShellClass classify_shell(const std::string& command, const std::string& /*workspace*/) {
    const std::vector<Tok> toks = tokenize_command(command);
    if (toks.empty())
        return {ShellEffect::ReadOnly, ""}; // nothing to run

    // Escapes anywhere (unquoted &, `, $(), glued operators) mean the command
    // backgrounds, substitutes, or chains without whitespace — none of it can
    // be proven safe.
    if (has_escape_token(toks))
        return {ShellEffect::Destructive, "bash:*"};

    const std::vector<std::pair<std::size_t, std::size_t>> segs = split_segments(toks);
    Accumulator acc;
    for (const auto& seg : segs) {
        SegmentVerdict v = classify_segment(toks, seg, acc.outside);
        if (v.unsafe_head)
            return {ShellEffect::Destructive, "bash:*"}; // qualified/unknown binary
        acc.merge(v);
    }
    return acc.verdict(segs.size() > 1);
}

} // namespace agent
