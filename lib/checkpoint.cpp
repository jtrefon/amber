#include "agent/checkpoint.h"

#include "agent/git_exec.h"
#include "agent/workspace.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace agent {

namespace {

namespace fs = std::filesystem;

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

// A ref component may not contain spaces, '~', '^', ':', '?', '*', '[', '\\',
// or '..'. Keep only what is unambiguous and cap the length.
std::string sanitize_session(const std::string& id) {
    std::string out;
    for (unsigned char c : id) {
        if (std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-')
            out.push_back(static_cast<char>(c));
        else
            out.push_back('-');
    }
    while (!out.empty() && out.front() == '.')
        out.erase(out.begin());
    while (!out.empty() && out.back() == '.')
        out.pop_back();
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
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        switch (text[++i]) {
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
            out.push_back(text[i]);
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

} // namespace

CheckpointStore::CheckpointStore(std::string project_root, std::string session_id)
    : session_(std::move(session_id)) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(project_root, ec);
    root_ = ec ? std::move(project_root) : absolute.lexically_normal().string();

    const GitResult inside = run_git(root_, {"rev-parse", "--is-inside-work-tree"});
    shadow_ = !(inside.status == 0 && trimmed(inside.output) == "true");
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
        env.push_back({"GIT_DIR", shadow_git_dir_});
        env.push_back({"GIT_WORK_TREE", root_});
    }
    return env;
}

bool CheckpointStore::ensure_shadow(std::string& error) const {
    if (!shadow_)
        return true;
    std::error_code ec;
    fs::create_directories(fs::path(shadow_git_dir_).parent_path(), ec);
    if (fs::exists(fs::path(shadow_git_dir_) / "HEAD"))
        return true;
    const GitResult init = run_git(root_, {"init", "--bare", "--quiet", shadow_git_dir_});
    if (init.status != 0) {
        error = "could not create the checkpoint repository: " + failure(init);
        return false;
    }
    return true;
}

bool CheckpointStore::read_ref(std::string& id) const {
    id.clear();
    const GitResult r = run_git(root_, {"rev-parse", "--verify", "--quiet", ref_}, environment());
    if (r.status != 0)
        return false;
    id = trimmed(r.output);
    return !id.empty();
}

std::optional<Checkpoint> CheckpointStore::create(std::size_t turn, const std::string& prompt,
                                                  std::string& error) {
    error.clear();
    if (!ensure_shadow(error))
        return std::nullopt;

    std::error_code ec;
    fs::create_directories(fs::path(index_path_).parent_path(), ec);
    // A fresh index from an empty state: the worktree is the snapshot, and no
    // real index entry or staged blob is consulted.
    fs::remove(index_path_, ec);
    const auto env = environment();
    const auto clean_index = [&] { fs::remove(index_path_, ec); };

    const GitResult add = run_git(root_, {"add", "-A"}, env);
    if (add.status != 0) {
        clean_index();
        error = "could not stage the working tree: " + failure(add);
        return std::nullopt;
    }
    const GitResult tree_result = run_git(root_, {"write-tree"}, env);
    if (tree_result.status != 0) {
        clean_index();
        error = "could not write the checkpoint tree: " + failure(tree_result);
        return std::nullopt;
    }
    const std::string tree = trimmed(tree_result.output);

    std::string parent;
    read_ref(parent);
    if (!parent.empty()) {
        const GitResult parent_tree = run_git(root_, {"rev-parse", parent + "^{tree}"}, env);
        if (parent_tree.status != 0) {
            clean_index();
            error = "could not read the previous checkpoint: " + failure(parent_tree);
            return std::nullopt;
        }
        if (trimmed(parent_tree.output) == tree) {
            clean_index();
            return std::nullopt; // a read-only turn
        }
    }

    const GitResult listing = parent.empty()
                                  ? run_git(root_, {"ls-tree", "-r", "--name-only", tree}, env)
                                  : run_git(root_, {"diff", "--name-only", parent, tree}, env);
    if (listing.status != 0) {
        clean_index();
        error = "could not list the changed files: " + failure(listing);
        return std::nullopt;
    }
    const std::size_t files = count_lines(listing.output);
    if (parent.empty() && files == 0) {
        clean_index();
        return std::nullopt; // an empty project changed nothing
    }

    const std::string subject = one_line(prompt, 72);
    std::string message = "amber: turn " + std::to_string(turn);
    if (!subject.empty())
        message += " — " + subject;
    message += "\nTurn: " + std::to_string(turn) + " | Session: " + sanitize_session(session_) +
               " | Files: " + std::to_string(files) + " changed";
    message += "\nPrompt: \"" + escape_prompt(prompt) + "\"";

    std::vector<std::string> commit_args{"commit-tree", tree};
    if (!parent.empty()) {
        commit_args.push_back("-p");
        commit_args.push_back(parent);
    }
    commit_args.push_back("-m");
    commit_args.push_back(message);
    const GitResult commit = run_git(root_, commit_args, env);
    if (commit.status != 0) {
        clean_index();
        error = "could not write the checkpoint commit: " + failure(commit);
        return std::nullopt;
    }
    const std::string id = trimmed(commit.output);

    const std::string zero(40, '0');
    const GitResult update =
        run_git(root_, {"update-ref", ref_, id, parent.empty() ? zero : parent}, env);
    if (update.status != 0) {
        clean_index();
        error = "could not update " + ref_ + ": " + failure(update);
        return std::nullopt;
    }
    clean_index();

    Checkpoint checkpoint;
    checkpoint.id = id;
    checkpoint.ref = ref_;
    checkpoint.turn = turn;
    checkpoint.prompt = prompt;
    checkpoint.files = files;
    const GitResult timestamp = run_git(root_, {"show", "-s", "--format=%ct", id}, env);
    if (timestamp.status == 0)
        checkpoint.timestamp_s = std::atoll(trimmed(timestamp.output).c_str());
    return checkpoint;
}

std::vector<Checkpoint> CheckpointStore::list(std::string& error) const {
    error.clear();
    std::vector<Checkpoint> out;
    if (shadow_ && !fs::exists(fs::path(shadow_git_dir_) / "HEAD"))
        return out;
    const auto env = environment();
    const GitResult revisions = run_git(root_, {"rev-list", ref_}, env);
    if (revisions.status != 0) {
        const std::string text = trimmed(revisions.output);
        // An absent ref is an empty history, not a broken repository.
        if (text.find("unknown revision") == std::string::npos &&
            text.find("bad revision") == std::string::npos &&
            text.find("Needed a single revision") == std::string::npos && !text.empty())
            error = text;
        return out;
    }
    for (const std::string& id : split_lines(revisions.output)) {
        const GitResult shown = run_git(root_, {"show", "-s", "--format=%ct%x1f%B", id}, env);
        if (shown.status != 0) {
            error = failure(shown);
            return {};
        }
        Checkpoint checkpoint;
        checkpoint.id = id;
        checkpoint.ref = ref_;
        const std::size_t separator = shown.output.find('\x1f');
        checkpoint.timestamp_s = std::atoll(shown.output.substr(0, separator).c_str());
        const std::string body =
            separator == std::string::npos ? std::string() : shown.output.substr(separator + 1);
        parse_message(body, checkpoint);
        out.push_back(std::move(checkpoint));
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
    for (const auto& checkpoint : checkpoints) {
        if (numeric) {
            if (checkpoint.turn == std::strtoul(turn_or_sha.c_str(), nullptr, 10))
                return checkpoint;
        } else if (!turn_or_sha.empty() && checkpoint.id.rfind(turn_or_sha, 0) == 0) {
            return checkpoint;
        }
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
    const GitResult result =
        run_git(root_, {"diff", "--no-color", "--no-ext-diff", base, checkpoint.id}, env);
    if (result.status != 0) {
        error = failure(result);
        return {};
    }
    return result.output;
}

} // namespace agent
