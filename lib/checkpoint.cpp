#include "agent/checkpoint.h"

#include "agent/git_exec.h"
#include "agent/workspace.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace agent {

namespace {

namespace fs = std::filesystem;

// An internal git failure. The public API turns it into its `error` out-param;
// an exception keeps the happy path straight-line and gives every call site the
// same message shape.
class GitFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (!line.empty())
            out.push_back(line);
    }
    return out;
}

std::size_t count_lines(const std::string& text) {
    return split_lines(text).size();
}

// `git ls-files -z` / `ls-tree -z` output: NUL-separated, no quoting, so a
// filename with a newline or a quote is one path.
std::vector<std::string> split_nul(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\0', start);
        if (end == std::string::npos) {
            out.push_back(text.substr(start));
            break;
        }
        if (end > start)
            out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

// A deleted file can leave its parent directories empty. Git does not track
// directories, so removing them keeps the tree looking like the snapshot.
void remove_empty_parents(const std::string& root, const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    while (!parent.empty() && parent != ".") {
        const std::filesystem::path dir = std::filesystem::path(root) / parent;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) || !std::filesystem::is_empty(dir, ec))
            break;
        std::filesystem::remove(dir, ec);
        if (ec)
            break;
        parent = parent.parent_path();
    }
}

// A ref component may not contain spaces, '~', '^', ':', '?', '*', '[', '\\',
// or '..'. Allow only alphanumerics, '_' and '-', so no dot can form a
// forbidden sequence, and cap the length.
std::string sanitize_session(const std::string& id) {
    std::string out;
    for (unsigned char c : id) {
        if (std::isalnum(c) != 0 || c == '_' || c == '-')
            out.push_back(static_cast<char>(c));
        else
            out.push_back('-');
    }
    if (out.size() > 64)
        out.resize(64);
    return out.empty() ? "default" : out;
}

// Truncate at a UTF-8 boundary: a cut in the middle of a codepoint would put
// invalid bytes in a commit message a later parse has to read.
std::string truncate_utf8(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes)
        return text;
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    text.resize(end);
    return text;
}

std::string one_line(const std::string& text, std::size_t max_bytes) {
    std::string out;
    out.reserve(std::min(text.size(), max_bytes));
    bool last_space = false;
    for (unsigned char c : text) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!out.empty() && !last_space)
                out.push_back(' ');
            last_space = true;
            continue;
        }
        out.push_back(static_cast<char>(c));
        last_space = false;
    }
    return truncate_utf8(trimmed(out), max_bytes);
}

std::string escape_prompt(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::string unescape_prompt(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i++];
        if (c != '\\' || i >= text.size()) {
            out.push_back(c);
            continue;
        }
        const char escaped = text[i++];
        switch (escaped) {
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '"':
            out.push_back('"');
            break;
        default:
            out.push_back('\\');
            out.push_back(escaped);
            break;
        }
    }
    return out;
}

void parse_message(const std::string& body, Checkpoint& checkpoint) {
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("Turn: ", 0) == 0) {
            checkpoint.turn = std::strtoul(line.c_str() + 6, nullptr, 10);
            const auto files = line.find("Files: ");
            if (files != std::string::npos)
                checkpoint.files = std::strtoul(line.c_str() + files + 7, nullptr, 10);
        } else if (line == "Phase: pre") {
            checkpoint.phase = CheckpointPhase::Pre;
        } else if (line.rfind("Prompt: ", 0) == 0) {
            std::string value = line.substr(8);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            checkpoint.prompt = unescape_prompt(value);
        }
    }
}

std::string failure(const GitResult& result) {
    const std::string text = trimmed(result.output);
    return text.empty() ? "git exited with status " + std::to_string(result.status) : text;
}

// One git call whose failure is exceptional for the operation being performed.
std::string git_checked(const std::string& root, const std::vector<std::string>& args,
                        const std::vector<std::pair<std::string, std::string>>& env,
                        const std::string& what) {
    const GitResult result = run_git(root, args, env);
    if (result.status != 0)
        throw GitFailure(what + ": " + failure(result));
    return result.output;
}

bool is_absent_ref(const std::string& message) {
    return message.find("unknown revision") != std::string::npos ||
           message.find("bad revision") != std::string::npos ||
           message.find("Needed a single revision") != std::string::npos;
}

struct IndexCleanup {
    const std::string& path;
    ~IndexCleanup() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

} // namespace

CheckpointStore::CheckpointStore(std::string project_root, std::string session_id)
    : session_(std::move(session_id)) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(project_root, ec);
    root_ = ec ? std::move(project_root) : absolute.lexically_normal().string();

    const GitResult inside = run_git(root_, {"rev-parse", "--is-inside-work-tree"});
    shadow_ = inside.status != 0 || trimmed(inside.output) != "true";
    if (!shadow_) {
        const GitResult top = run_git(root_, {"rev-parse", "--show-toplevel"});
        if (top.status == 0 && !trimmed(top.output).empty())
            root_ = trimmed(top.output);
    }

    const std::string safe = sanitize_session(session_);
    ref_ = "refs/amber/checkpoints/" + safe;
    const fs::path base = fs::path(Workspace::state_dir_for(root_)) / "checkpoints";
    index_path_ = (base / (safe + ".index")).string();
    if (shadow_)
        shadow_git_dir_ = (base / (safe + ".git")).string();
}

std::vector<std::pair<std::string, std::string>> CheckpointStore::environment() const {
    std::vector<std::pair<std::string, std::string>> env{
        {"GIT_INDEX_FILE", index_path_},
        {"GIT_AUTHOR_NAME", "amber"},
        {"GIT_AUTHOR_EMAIL", "amber@localhost"},
        {"GIT_COMMITTER_NAME", "amber"},
        {"GIT_COMMITTER_EMAIL", "amber@localhost"},
    };
    if (shadow_) {
        env.emplace_back("GIT_DIR", shadow_git_dir_);
        env.emplace_back("GIT_WORK_TREE", root_);
    }
    return env;
}

void CheckpointStore::ensure_shadow() const {
    if (!shadow_)
        return;
    std::error_code ec;
    fs::create_directories(fs::path(shadow_git_dir_).parent_path(), ec);
    if (fs::exists(fs::path(shadow_git_dir_) / "HEAD"))
        return;
    git_checked(root_, {"init", "--bare", "--quiet", shadow_git_dir_}, {},
                "could not create the checkpoint repository");
}

std::optional<std::string> CheckpointStore::read_ref() const {
    const GitResult r = run_git(root_, {"rev-parse", "--verify", "--quiet", ref_}, environment());
    if (r.status != 0)
        return std::nullopt;
    std::string id = trimmed(r.output);
    if (id.empty())
        return std::nullopt;
    return id;
}

std::string CheckpointStore::stage_tree() const {
    std::error_code ec;
    fs::create_directories(fs::path(index_path_).parent_path(), ec);
    // A fresh index from an empty state: the worktree is the snapshot, and no
    // real index entry or staged blob is consulted.
    fs::remove(index_path_, ec);
    const auto env = environment();
    git_checked(root_, {"add", "-A"}, env, "could not stage the working tree");
    return trimmed(git_checked(root_, {"write-tree"}, env, "could not write the checkpoint tree"));
}

bool CheckpointStore::same_as_parent(const std::string& parent, const std::string& tree) const {
    const std::string parent_tree =
        git_checked(root_, {"rev-parse", parent + "^{tree}"}, environment(),
                    "could not read the previous checkpoint");
    return trimmed(parent_tree) == tree;
}

std::size_t CheckpointStore::changed_files(const std::string& parent,
                                           const std::string& tree) const {
    const auto env = environment();
    const std::string listing = parent.empty()
                                    ? git_checked(root_, {"ls-tree", "-r", "--name-only", tree},
                                                  env, "could not list the changed files")
                                    : git_checked(root_, {"diff", "--name-only", parent, tree}, env,
                                                  "could not list the changed files");
    return count_lines(listing);
}

std::string CheckpointStore::commit_message(std::size_t turn, const std::string& prompt,
                                            std::size_t files, CheckpointPhase phase) const {
    const std::string subject = one_line(prompt, 72);
    std::string message;
    if (phase == CheckpointPhase::Pre) {
        // The prompt already names the upcoming turn ("before turn 2"), so the
        // subject is the prompt rather than a second turn label.
        message =
            "amber: " + (subject.empty() ? "before turn " + std::to_string(turn + 1) : subject);
    } else {
        message = "amber: turn " + std::to_string(turn);
        if (!subject.empty())
            message += " — " + subject;
    }
    message += "\nTurn: " + std::to_string(turn) + " | Session: " + sanitize_session(session_) +
               " | Files: " + std::to_string(files) + " changed";
    message += "\nPhase: ";
    message += phase == CheckpointPhase::Pre ? "pre" : "post";
    message += "\nPrompt: \"" + escape_prompt(prompt) + "\"";
    return message;
}

std::string CheckpointStore::write_commit(const std::string& tree, const std::string& parent,
                                          const std::string& message) const {
    std::vector<std::string> args{"commit-tree", tree};
    if (!parent.empty()) {
        args.emplace_back("-p");
        args.push_back(parent);
    }
    args.emplace_back("-m");
    args.push_back(message);
    return trimmed(
        git_checked(root_, args, environment(), "could not write the checkpoint commit"));
}

void CheckpointStore::move_ref(const std::string& id, const std::string& parent) const {
    const std::string zero(40, '0');
    git_checked(root_, {"update-ref", ref_, id, parent.empty() ? zero : parent}, environment(),
                "could not update " + ref_);
}

std::optional<Checkpoint> CheckpointStore::create(std::size_t turn, const std::string& prompt,
                                                  std::string& error, CheckpointPhase phase) {
    error.clear();
    try {
        ensure_shadow();
        const IndexCleanup cleanup{index_path_};
        const std::string tree = stage_tree();
        const auto parent = read_ref();
        if (parent && same_as_parent(*parent, tree))
            return std::nullopt; // a read-only turn
        const std::string base = parent.value_or("");
        const std::size_t files = changed_files(base, tree);
        if (base.empty() && files == 0)
            return std::nullopt; // an empty project changed nothing

        const std::string id = write_commit(tree, base, commit_message(turn, prompt, files, phase));
        move_ref(id, base);

        Checkpoint checkpoint;
        checkpoint.id = id;
        checkpoint.ref = ref_;
        checkpoint.turn = turn;
        checkpoint.prompt = prompt;
        checkpoint.files = files;
        checkpoint.phase = phase;
        const GitResult timestamp =
            run_git(root_, {"show", "-s", "--format=%ct", id}, environment());
        if (timestamp.status == 0)
            checkpoint.timestamp_s = std::atoll(trimmed(timestamp.output).c_str());
        return checkpoint;
    } catch (const GitFailure& failure) {
        error = failure.what();
        return std::nullopt;
    }
}

std::vector<Checkpoint> CheckpointStore::list(std::string& error) const {
    error.clear();
    std::vector<Checkpoint> out;
    if (shadow_ && !fs::exists(fs::path(shadow_git_dir_) / "HEAD"))
        return out;
    try {
        const auto env = environment();
        const std::string revisions =
            git_checked(root_, {"rev-list", ref_}, env, "could not list the checkpoints");
        for (const std::string& id : split_lines(revisions)) {
            const std::string shown = git_checked(root_, {"show", "-s", "--format=%ct%x1f%B", id},
                                                  env, "could not read checkpoint " + id);
            Checkpoint checkpoint;
            checkpoint.id = id;
            checkpoint.ref = ref_;
            const std::size_t separator = shown.find('\x1f');
            checkpoint.timestamp_s = std::atoll(shown.substr(0, separator).c_str());
            const std::string body =
                separator == std::string::npos ? std::string() : shown.substr(separator + 1);
            parse_message(body, checkpoint);
            out.push_back(std::move(checkpoint));
        }
    } catch (const GitFailure& failure) {
        if (is_absent_ref(failure.what()))
            return out;
        error = failure.what();
        return {};
    }
    return out;
}

std::optional<Checkpoint> CheckpointStore::find(const std::string& turn_or_sha,
                                                std::string& error) const {
    const auto checkpoints = list(error);
    if (!error.empty())
        return std::nullopt;
    const bool numeric =
        !turn_or_sha.empty() && std::all_of(turn_or_sha.begin(), turn_or_sha.end(),
                                            [](unsigned char c) { return std::isdigit(c) != 0; });
    // A turn number wins when it exists. A SHA prefix may also be all digits
    // (roughly one in forty), so a numeric selector falls through to the SHA
    // match rather than deciding the selector's kind by its characters alone.
    if (numeric) {
        const std::size_t turn = std::strtoul(turn_or_sha.c_str(), nullptr, 10);
        // The post snapshot is the turn's result; a pre snapshot that shares
        // the number is the state the next turn started from. The turn selector
        // means the result, so the pre entry is the fallback.
        for (const auto& checkpoint : checkpoints)
            if (checkpoint.turn == turn && !checkpoint.pre())
                return checkpoint;
        for (const auto& checkpoint : checkpoints)
            if (checkpoint.turn == turn)
                return checkpoint;
    }
    if (!turn_or_sha.empty()) {
        for (const auto& checkpoint : checkpoints)
            if (checkpoint.id.rfind(turn_or_sha, 0) == 0)
                return checkpoint;
    }
    return std::nullopt;
}

std::string CheckpointStore::diff(const Checkpoint& checkpoint, std::string& error) const {
    error.clear();
    const auto env = environment();
    std::string base = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"; // the empty tree
    const GitResult parent =
        run_git(root_, {"rev-parse", "--verify", "--quiet", checkpoint.id + "^"}, env);
    if (parent.status == 0 && !trimmed(parent.output).empty())
        base = trimmed(parent.output);
    try {
        return git_checked(root_, {"diff", "--no-color", "--no-ext-diff", base, checkpoint.id}, env,
                           "could not diff the checkpoint");
    } catch (const GitFailure& failure) {
        error = failure.what();
        return {};
    }
}

bool CheckpointStore::restore(const Checkpoint& checkpoint, std::string& error) const {
    error.clear();
    try {
        const auto env = environment();
        const IndexCleanup cleanup{index_path_};
        std::error_code ec;
        fs::create_directories(fs::path(index_path_).parent_path(), ec);
        fs::remove(index_path_, ec);

        // Stage the current worktree into the private index so the current
        // paths come from git's own ignore rules, and the user's real index is
        // never read. The same index is then replaced by the target tree.
        git_checked(root_, {"add", "-A"}, env, "could not stage the working tree");
        const auto current = split_nul(
            git_checked(root_, {"ls-files", "-z"}, env, "could not list the working tree"));

        fs::remove(index_path_, ec);
        git_checked(root_, {"read-tree", checkpoint.id + "^{tree}"}, env,
                    "could not read the checkpoint tree");
        git_checked(root_, {"checkout-index", "-a", "-f"}, env,
                    "could not restore the checkpoint files");

        const auto target =
            split_nul(git_checked(root_, {"ls-tree", "-r", "--name-only", "-z", checkpoint.id}, env,
                                  "could not list the checkpoint files"));
        const std::unordered_set<std::string> keep(target.begin(), target.end());

        // Remove only what the snapshot did not contain. Ignored files were
        // never in the snapshot and never appear in `current`, so they stay.
        for (const auto& path : current) {
            if (keep.find(path) != keep.end())
                continue;
            std::error_code removed;
            fs::remove(fs::path(root_) / fs::path(path), removed);
            if (removed)
                throw GitFailure("could not remove " + path + ": " + removed.message());
            remove_empty_parents(root_, path);
        }
        return true;
    } catch (const GitFailure& failure) {
        error = failure.what();
        return false;
    }
}

} // namespace agent
