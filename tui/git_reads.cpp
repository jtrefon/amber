#include "git_reads.h"

#include <cstdio>
#include <string>

namespace tui::git_reads {
namespace {

// A workspace path reaches a shell here, so it is quoted rather than trusted to
// be free of spaces or quotes (a worktree sits under $TMPDIR, which a user may
// set to anything).
std::string shell_quoted(const std::string& s) {
    std::string out = "'";
    for (const char c : s)
        out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string read_stdout(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return result;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        result += buf;
    pclose(pipe);
    return result;
}

std::string git_dir(const std::string& root) {
    return root.empty() ? std::string(".") : shell_quoted(root);
}

// The pathspec matches the diff's exclusions, so the two reads describe the
// same tree.
constexpr const char* kPathspec = " -- . ':!bench/results' ':!*.o' ':!*.a' ':!*.d' 2>/dev/null";

} // namespace

text::GitStatus read_status(const std::string& root) {
    return text::parse_git_status_v2(
        read_stdout("git -C " + git_dir(root) + " status --porcelain=v2 --branch" + kPathspec));
}

text::DiffStat read_diff_stat(const std::string& root) {
    // A worktree whose line endings differ from the index -- a Windows checkout
    // read from WSL, where the two gits disagree on core.autocrlf -- makes every
    // line a deletion and an insertion. Line-ending churn is not a content
    // change, so the prompt's counts ignore CR at end of line; without this a
    // clean tree reports the whole repository as +N/-N.
    return text::parse_git_shortstat(read_stdout(
        "git -C " + git_dir(root) + " diff --shortstat --ignore-cr-at-eol" + kPathspec));
}

} // namespace tui::git_reads
