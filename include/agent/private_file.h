#ifndef AGENT_PRIVATE_FILE_H
#define AGENT_PRIVATE_FILE_H

#include <string>

namespace agent {

// Owner-only file and directory primitives for anything that can hold a
// credential, a conversation transcript, a session or a policy snapshot.
//
// Every write opens with mode 0600 at creation time (never chmod-after-write,
// which leaves a readable window), every directory is narrowed to 0700, and a
// symlink is never followed: the path is either the real file or the write
// fails. Existing files are narrowed in place by secure_existing_file, so a
// pre-fix tree is repaired on the next startup without touching its content.

// Create `path` (and its parents) if needed, then narrow it to 0700.
// Returns false when the directory cannot be created or narrowed.
bool ensure_private_dir(const std::string& path);

// Open a private append/truncate stream. Returns -1 on failure, including a
// symlink at `path`. The caller owns the descriptor.
int open_private_append(const std::string& path);
int open_private_truncate(const std::string& path);

// Write every byte, retrying on EINTR. Returns false on error.
bool write_all(int fd, const std::string& bytes);

// Atomically replace `path` with `bytes` (temp file in the same directory,
// fsync, rename). The temporary and the result are 0600.
bool write_private_file(const std::string& path, const std::string& bytes);

// Narrow an existing regular file to 0600 / an existing directory to 0700.
// A missing path is a no-op (true); a symlink is refused (false).
bool secure_existing_file(const std::string& path);
bool secure_existing_dir(const std::string& path);

// One startup pass over amber's known private paths: global config, provider
// files, sessions, transcripts directory, error journal, artifacts directory,
// settings, policy and the experience store. Idempotent, bounded (it does not
// walk the transcript files), and never blocks on a failure.
void secure_private_state();

// The same pass on a background worker, for hosts that must paint first. The
// future is process-static, so it is joined at exit; the worker only touches
// filesystem modes.
void secure_private_state_async();

} // namespace agent

#endif // AGENT_PRIVATE_FILE_H
