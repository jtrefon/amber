#ifndef AGENT_CHECKPOINT_H
#define AGENT_CHECKPOINT_H

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agent {

// One per-session snapshot of the working tree.
struct Checkpoint {
    std::string id; // commit SHA
    std::string ref;
    std::size_t turn = 0;
    std::string prompt;
    std::size_t files = 0;
    long long timestamp_s = 0;
};

// Per-session snapshots stored as commits on `refs/amber/checkpoints/<session>`.
// The user's branch, HEAD and index never move: the snapshot stages through a
// private index and updates only its own ref.
//
// A project that is not a git checkout gets a private bare repository under the
// state directory, with the project as its work tree. Nothing is written inside
// the project, so there is no `.git` to confuse a later `git init` and no
// `.gitignore` entry to maintain.
//
// One store belongs to one agent worker; the ref and the private index are per
// session, so two windows on one repository keep independent histories.
class CheckpointStore {
public:
    CheckpointStore(std::string project_root, std::string session_id);

    // Snapshot the working tree after `turn`. Returns nullopt with an empty
    // `error` when the tree is identical to the previous checkpoint (a
    // read-only turn); a non-empty `error` means the snapshot could not be
    // taken. Commits are authored as amber, independent of the user's git
    // identity.
    std::optional<Checkpoint> create(std::size_t turn, const std::string& prompt,
                                     std::string& error);

    // Newest first.
    std::vector<Checkpoint> list(std::string& error) const;

    // `turn_or_sha` is a turn number (all digits) or a commit SHA prefix.
    std::optional<Checkpoint> find(const std::string& turn_or_sha, std::string& error) const;

    // The unified diff this checkpoint introduced, against its parent (the
    // empty tree for the first checkpoint).
    std::string diff(const Checkpoint& checkpoint, std::string& error) const;

    const std::string& ref() const noexcept { return ref_; }
    bool shadow() const noexcept { return shadow_; }

private:
    std::vector<std::pair<std::string, std::string>> environment() const;
    void ensure_shadow() const;
    std::optional<std::string> read_ref() const;
    std::string stage_tree() const;
    bool same_as_parent(const std::string& parent, const std::string& tree) const;
    std::size_t changed_files(const std::string& parent, const std::string& tree) const;
    std::string commit_message(std::size_t turn, const std::string& prompt,
                               std::size_t files) const;
    std::string write_commit(const std::string& tree, const std::string& parent,
                             const std::string& message) const;
    void move_ref(const std::string& id, const std::string& parent) const;

    std::string root_; // the work tree: repo top-level when there is one
    std::string ref_;
    std::string index_path_;
    std::string shadow_git_dir_;
    std::string session_;
    bool shadow_ = false;
};

} // namespace agent

#endif // AGENT_CHECKPOINT_H
