#include "tui/draft_store.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace tui::draft_store {

namespace fs = std::filesystem;

std::string path(const std::string& workspace_root) {
    if (workspace_root.empty())
        return {};
    return (fs::path(workspace_root) / ".amber" / "draft").string();
}

std::string load(const std::string& workspace_root) {
    const std::string p = path(workspace_root);
    if (p.empty())
        return {};
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool save(const std::string& workspace_root, const std::string& text) {
    const std::string p = path(workspace_root);
    if (p.empty())
        return false;
    if (text.empty()) {
        clear(workspace_root);
        return true;
    }
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);
    if (ec)
        return false;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << text;
    const bool ok = out.good();
    // A draft can contain anything the user was about to send; keep it private.
    std::error_code pec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                    pec);
    return ok;
}

void clear(const std::string& workspace_root) {
    const std::string p = path(workspace_root);
    if (p.empty())
        return;
    std::error_code ec;
    fs::remove(p, ec);
}

void DraftKeeper::seed(const std::string& workspace_root, const std::string& text) {
    root_ = workspace_root;
    seen_ = text;
    pending_ = false;
}

void DraftKeeper::update(const std::string& workspace_root, const std::string& text,
                         Clock::time_point now) {
    if (workspace_root != root_) {
        flush();
        root_ = workspace_root;
        seen_.clear();
        pending_ = false;
    }
    if (text != seen_) {
        seen_ = text;
        pending_ = !text.empty();
        due_ = now + std::chrono::milliseconds(500);
        if (text.empty())
            clear();
        return;
    }
    if (pending_ && now >= due_)
        flush();
}

void DraftKeeper::flush() {
    pending_ = false;
    if (seen_.empty() || root_.empty())
        return;
    save(root_, seen_);
}

void DraftKeeper::save_now(const std::string& workspace_root, const std::string& text) {
    seed(workspace_root, text);
    pending_ = !text.empty();
    flush();
}

void DraftKeeper::clear() {
    pending_ = false;
    seen_.clear();
    if (!root_.empty())
        draft_store::clear(root_);
}

} // namespace tui::draft_store
