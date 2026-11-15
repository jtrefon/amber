#ifndef AMBER_TUI_DRAFT_STORE_H
#define AMBER_TUI_DRAFT_STORE_H

#include <chrono>
#include <string>

// L1 (pure I/O): the unsent prompt's per-workspace draft.
//
// The file is <workspace>/.amber/draft, the raw text only. Every path is
// best-effort: a missing, unreadable or unwritable draft is not an error the
// UI should report, because losing a draft must never stop the composer from
// working.
namespace tui::draft_store {

std::string path(const std::string& workspace_root);

// The draft text, or "" when there is none (or it cannot be read).
std::string load(const std::string& workspace_root);

// Write the draft, creating <workspace>/.amber when needed. Returns false when
// the write failed; an empty text clears the file instead of storing it.
bool save(const std::string& workspace_root, const std::string& text);

void clear(const std::string& workspace_root);

// The write policy: one file per workspace, written 500 ms after the last
// change. The clock is a parameter, so the debounce is testable without a
// terminal or a sleep.
class DraftKeeper {
public:
    using Clock = std::chrono::steady_clock;

    DraftKeeper() = default;
    // Start tracking a restored draft without writing it back immediately.
    DraftKeeper(const std::string& workspace_root, const std::string& text) {
        seed(workspace_root, text);
    }

    // Start tracking `text` without writing it (used after a restore, so a
    // draft loaded from disk is not immediately written back).
    void seed(const std::string& workspace_root, const std::string& text);

    // Note the current text. Writes when the debounce expired; changing
    // workspace flushes the old one first.
    void update(const std::string& workspace_root, const std::string& text, Clock::time_point now);

    // Write whatever is pending, if anything.
    void flush();

    // Write the current text immediately (quit), or clear the file (submit).
    void save_now(const std::string& workspace_root, const std::string& text);
    void clear();

private:
    std::string root_;
    std::string seen_;
    Clock::time_point due_{};
    bool pending_ = false;
};

} // namespace tui::draft_store

#endif // AMBER_TUI_DRAFT_STORE_H
