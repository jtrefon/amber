
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

// Split a command line honoring single/double quotes. Quoted spans become a
// single token flagged quoted; quote characters are stripped.
std::vector<Tok> tokenize(const std::string& s) {
    std::vector<Tok> out;
    std::string cur;
    bool quoted = false;
    bool in_single = false, in_double = false;
    for (char c : s) {
        if (in_single) {
            if (c == '\'') in_single = false;
            else cur += c;
            quoted = true;
            continue;
        }
        if (in_double) {
            if (c == '"') in_double = false;
            else cur += c;
            quoted = true;
            continue;
        }
        if (c == '\'') { in_single = true; continue; }
        if (c == '"') { in_double = true; continue; }
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty()) { out.push_back({cur, quoted}); cur.clear(); quoted = false; }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back({cur, quoted});
    return out;
}

std::vector<std::string> pattern_words(const std::string& pattern) {
    std::vector<std::string> w;
    for (const auto& t : tokenize(pattern)) w.push_back(t.text);
    return w;
}

// Heads whose arguments are never executed and never name write targets:
// they only read or print, so path-like arguments (even /etc/...) are data.
bool is_reader_head(const std::string& h) {
    static const char* kReaders[] = {
        "ls", "cat", "grep", "head", "tail", "wc", "sort", "uniq",
        "diff", "pwd", "which", "env", "printenv", "date", "echo",
        "printf", "cd", "export"};
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
    if (t.quoted) return false;
    const std::string& s = t.text;
    if (s == "&" || s == "`" || s.rfind("$(", 0) == 0) return true;
    if (s.rfind("2>&", 0) == 0 || s.rfind("1>&", 0) == 0) return false;
    return std::any_of(s.begin(), s.end(), [](char c) {
        return c == ';' || c == '&' || c == '|' || c == '`';
    });
}

// Output redirection (">", ">>", ">f", "2>f", "&>f"): the command writes a
// file. Input redirects ("<", "<f") read; fd dups (2>&1) redirect within the
// process. Returns the target path ("" when the target is the next token).
bool output_redirect_target(const Tok& t, std::string& target) {
    if (t.quoted) return false;
    const std::string& s = t.text;
    std::size_t i = std::string::npos;
    if (s == ">" || s == ">>" || s == "&>") return true;  // target follows
    if (s == "2>" || s == "2>>" || s == "1>" || s == "1>>") return true;
    if (s[0] == '>') i = 1;
    else if (s.rfind("2>", 0) == 0 || s.rfind("1>", 0) == 0 ||
             s.rfind("&>", 0) == 0)
        i = 2;
    if (i == std::string::npos) return false;
    while (i < s.size() && s[i] == '>') ++i;
    target = s.substr(i);
    return true;
}

bool is_input_redirect(const Tok& t) {
    if (t.quoted) return false;
    const std::string& s = t.text;
    if (s == "<") return true;
    if (s[0] == '<') return true;
    if (s.rfind("0<", 0) == 0) return true;
    return false;
}

// Skip leading `VAR=value` assignments; returns the first command token index.
std::size_t first_command_token(const std::vector<Tok>& toks) {
    std::size_t i = 0;
    while (i < toks.size()) {
        const std::string& t = toks[i].text;
        std::size_t eq = t.find('=');
        if (eq == std::string::npos || eq == 0) break;
        bool name_ok = true;
        for (std::size_t k = 0; k < eq; ++k)
            if (!isalnum(static_cast<unsigned char>(t[k])) && t[k] != '_') {
                name_ok = false;
                break;
            }
        if (!name_ok) break;
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

// If an unquoted token names a path that escapes the workspace root, return
// the folder scope id ("outside:/abs/dir"); otherwise "". The workspace root
// is read from Workspace::root() — the parameter keeps the signature explicit
// for tests and matches the policy engine's calls.
std::string outside_scope_for(const Tok& t, bool is_target) {
    const std::string& s = t.text;
    if (s.empty() || s[0] == '-' || s[0] == '$') return "";
    if (t.quoted && !is_target) return "";   // quoted data, not a path
    bool path_like = s.find('/') != std::string::npos || s == "." ||
                     s == ".." || s.rfind("~/", 0) == 0 ||
                     s.rfind("../", 0) == 0;
    if (!is_target && !path_like) return "";  // bare word resolved by cwd
    if (is_target && !path_like && !t.quoted) return "";  // bare file name
    std::string resolved, err;
    if (Workspace::confine(s, resolved, err)) return "";
    return "outside:" + containing_dir(s);
}

bool pattern_matches(const std::string& pattern,
                     const std::vector<std::string>& ws, std::size_t start) {
    std::vector<std::string> want_words = pattern_words(pattern);
    if (want_words.empty() || start + want_words.size() > ws.size())
        return false;
    for (std::size_t i = 0; i < want_words.size(); ++i)
        if (ws[start + i] != want_words[i]) return false;
    return true;
}

} // namespace

const std::vector<std::string>& destructive_command_patterns() {
    // Curated destructive / system-wide command list. These ALWAYS prompt in
    // WRITE mode (subject to stored allow/deny rules). The list is the seed
    // for the JSON policy store and is user-editable there. Benign commands
    // like `cp`/`mv`/`touch` are deliberately absent — they are confined to
    // the workspace and run free unless their arguments escape it.
    static const std::vector<std::string> kPatterns = {
        "rm", "rmdir", "dd", "mkfs", "fdisk", "parted", "shred",
        "chmod -R", "chown -R", "sudo", "apt", "apt-get", "dnf", "yum",
        "pacman", "docker", "podman", "systemctl", "service", "shutdown",
        "reboot", "halt", "poweroff", "kill", "pkill", "killall",
        "git reset", "git clean", "git push --force", "git push -f",
        "git push", "git revert", "git checkout --", "git merge --abort",
        "pip install", "npm install", "npm publish", "yarn add",
        "cargo install",
    };
    return kPatterns;
}

ShellClass classify_shell(const std::string& command,
                          const std::string& /*workspace*/) {
    ShellClass out;
    std::vector<Tok> toks = tokenize(command);
    if (toks.empty()) {
        out.effect = ShellEffect::ReadOnly;  // nothing to run
        return out;
    }

    // Escape tokens anywhere (unquoted &, `, $(), glued operators) mean the
    // command backgrounds, substitutes, or chains without whitespace — none
    // of it can be proven safe.
    for (const auto& t : toks)
        if (is_escape_token(t)) {
            out.effect = ShellEffect::Destructive;
            out.scope_id = "bash:*";
            return out;
        }

    // Split into segments at chain operators (; && || |). Each segment is a
    // command of its own; the whole command is never ReadOnly if it composes.
    std::vector<std::pair<std::size_t, std::size_t>> segs;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (is_chain_op(toks[i].text)) {
            segs.emplace_back(begin, i);
            begin = i + 1;
        }
    }
    segs.emplace_back(begin, toks.size());
    bool composed = segs.size() > 1;

    std::string first_destructive;
    std::string first_outside;
    std::string first_write;
    bool any_write = false;

    for (const auto& seg : segs) {
        std::size_t head_i = first_command_token(
            std::vector<Tok>(toks.begin() + static_cast<long>(seg.first),
                             toks.begin() + static_cast<long>(seg.second)));
        std::size_t h = seg.first + head_i;
        if (h >= seg.second) continue;  // empty segment (e.g. trailing ";")
        const std::string& head = toks[h].text;
        if (head.empty() || head[0] == '-' ||
            head.find('/') != std::string::npos) {
            out.effect = ShellEffect::Destructive;  // qualified/unknown binary
            out.scope_id = "bash:*";
            return out;
        }

        std::vector<std::string> ws;
        for (std::size_t i = seg.first; i < seg.second; ++i)
            ws.push_back(toks[i].text);

        // git is a reader for its narrow read-only subcommands.
        bool git_reader = false;
        if (head == "git" && h + 1 < seg.second) {
            const std::string& sub = toks[h + 1].text;
            static const char* kReadSub[] = {"status", "diff", "log", "show",
                                             "branch", "remote", "ls-files",
                                             "rev-parse", "help"};
            git_reader = std::any_of(std::begin(kReadSub), std::end(kReadSub),
                                     [&](const char* s) { return sub == s; });
        }
        bool reader = is_reader_head(head) || git_reader;

        // Path and redirect scan for this segment.
        bool seg_write = false;
        std::string seg_scope;   // e.g. "bash:git add" for write grants
        bool target_next = false;
        for (std::size_t i = seg.first; i < seg.second; ++i) {
            const Tok& t = toks[i];
            if (target_next) {
                std::string scope = outside_scope_for(t, /*is_target=*/true);
                if (!scope.empty()) {
                    if (first_outside.empty()) first_outside = scope;
                }
                target_next = false;
                continue;
            }
            std::string rtarget;
            if (output_redirect_target(t, rtarget)) {
                seg_write = true;   // a redirect writes a file
                if (rtarget.empty()) target_next = true;
                else {
                    Tok target{rtarget, false};
                    std::string scope = outside_scope_for(target, true);
                    if (!scope.empty() && first_outside.empty())
                        first_outside = scope;
                }
                continue;
            }
            if (is_input_redirect(t)) continue;  // reads are not writes
            if (i == h) continue;                // the head itself
            // find -delete / -exec and sed -i mutate.
            if (head == "find" &&
                (t.text == "-delete" || t.text == "-exec" ||
                 t.text == "-execdir" || t.text == "-ok")) {
                seg_write = true;
                continue;
            }
            if (head == "sed" &&
                (t.text == "-i" || t.text.rfind("-i", 0) == 0)) {
                seg_write = true;
                continue;
            }
            if (reader) continue;  // args of readers are data/read targets
            // Write-capable head: an escaping path argument prompts per folder.
            std::string scope = outside_scope_for(t, /*is_target=*/false);
            if (!scope.empty()) {
                if (first_outside.empty()) first_outside = scope;
                continue;
            }
        }

        // Destructive pattern at this segment's head (e.g. rm, git reset).
        std::string dpat;
        for (const auto& pat : destructive_command_patterns()) {
            std::vector<std::string> pw = pattern_words(pat);
            if (pattern_matches(pat, ws, h - seg.first)) {
                dpat = pat;
                break;
            }
        }

        if (!reader && (dpat.empty() || first_outside.empty()) && !seg_write) {
            // A write-capable head with no flag/redirect/path is still a write
            // only when it is a known mutator or unknown; readers are handled
            // above. git non-read subcommands get a subcommand scope.
            if (head == "git" && !git_reader && h + 1 < seg.second) {
                seg_write = true;
                seg_scope = "bash:git " + toks[h + 1].text;
            } else if (!is_reader_head(head)) {
                seg_write = true;
                seg_scope = "bash:" + head;
            }
        }

        if (!first_outside.empty()) continue;  // outside wins; keep scanning
        if (!dpat.empty()) {
            if (first_destructive.empty()) first_destructive = "bash:" + dpat;
            continue;
        }
        if (seg_write) {
            any_write = true;
            if (first_write.empty()) first_write = seg_scope.empty()
                                                       ? "bash:" + head
                                                       : seg_scope;
        }
    }

    if (!first_outside.empty()) {
        out.effect = ShellEffect::Outside;
        out.scope_id = first_outside;
        return out;
    }
    if (!first_destructive.empty()) {
        out.effect = ShellEffect::Destructive;
        out.scope_id = first_destructive;
        return out;
    }
    if (composed || any_write) {
        // Composed commands (pipes/sequences of readers) are not provably
        // read-only; benign in-workspace writes run free in WRITE mode.
        out.effect = ShellEffect::Write;
        out.scope_id = first_write.empty() ? "bash:*" : first_write;
        return out;
    }
    out.effect = ShellEffect::ReadOnly;
    return out;
}

} // namespace agent
