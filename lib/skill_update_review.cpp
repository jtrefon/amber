#include "agent/skill_update_review.h"

#include "agent/skill_install.h"

#include <algorithm>
#include <filesystem>

namespace agent {

namespace {

namespace fs = std::filesystem;

std::vector<std::string> without_manifest(std::vector<std::string> paths) {
    paths.erase(std::remove(paths.begin(), paths.end(), std::string("SKILL.md")), paths.end());
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

std::vector<std::string> difference(const std::vector<std::string>& from,
                                    const std::vector<std::string>& against) {
    std::vector<std::string> out;
    for (const auto& path : from) {
        if (!std::binary_search(against.begin(), against.end(), path))
            out.push_back(path);
    }
    return out;
}

std::string short_hash(const std::string& hash) {
    if (hash.size() <= 7)
        return hash.empty() ? "unknown" : hash;
    return hash.substr(0, 7);
}

std::string join_paths(const std::vector<std::string>& paths) {
    std::string out;
    for (const auto& path : paths) {
        if (!out.empty())
            out += ", ";
        out += path;
    }
    return out;
}

std::string file_line(const std::vector<std::string>& files, const char* verb) {
    if (files.empty())
        return {};
    return std::string(verb) + std::to_string(files.size()) + " file" +
           (files.size() == 1 ? "" : "s") + " (" + join_paths(files) + ")";
}

} // namespace

UpdateReview build_update_review(const LedgerEntry& entry, const SkillDetail& detail,
                                 const std::string& installed_body,
                                 const std::vector<std::string>& installed_files) {
    UpdateReview review;
    review.name = detail.ref.name.empty() ? entry.slug : detail.ref.name;
    review.from_hash = entry.hash;
    review.to_hash = detail.hash;
    review.body_changed = !detail.hash.empty() && detail.hash != entry.hash;
    review.local_modified =
        !entry.local_hash.empty() && skill_content_hash(installed_body) != entry.local_hash;

    review.body_diff = render_unified_diff("SKILL.md", installed_body, detail.body);
    review.body_counts = count_diff(diff_lines(installed_body, detail.body));

    std::vector<std::string> remote;
    remote.reserve(detail.assets.size());
    for (const auto& asset : detail.assets)
        remote.push_back(asset.path);

    const std::vector<std::string> remote_files = without_manifest(std::move(remote));
    const std::vector<std::string> local_files = without_manifest(installed_files);
    review.added_files = difference(remote_files, local_files);
    review.removed_files = difference(local_files, remote_files);

    std::string status =
        review.name + ": " + short_hash(review.from_hash) + " → " + short_hash(review.to_hash);
    if (review.body_changed)
        status += ", +" + std::to_string(review.body_counts.added) + " -" +
                  std::to_string(review.body_counts.removed) + " lines";
    if (!review.added_files.empty())
        status += ", " + std::to_string(review.added_files.size()) + " added";
    if (!review.removed_files.empty())
        status += ", " + std::to_string(review.removed_files.size()) + " removed";
    if (review.local_modified)
        status += " · local edits";
    review.status = status;
    return review;
}

std::vector<std::string> update_review_lines(const UpdateReview& review) {
    std::vector<std::string> lines;
    lines.emplace_back(review.name + "  ·  " + short_hash(review.from_hash) + "  →  " +
                       short_hash(review.to_hash));
    if (review.local_modified)
        lines.emplace_back("local edits: applying discards them; press x to override");
    if (!review.body_changed && review.added_files.empty() && review.removed_files.empty())
        lines.emplace_back("no changes: the remote matches what is installed");
    else
        lines.insert(lines.end(), review.body_diff.begin(), review.body_diff.end());

    const std::string added = file_line(review.added_files, "+");
    const std::string removed = file_line(review.removed_files, "-");
    if (!added.empty())
        lines.push_back(added);
    if (!removed.empty())
        lines.push_back(removed);
    return lines;
}

std::vector<std::string> installed_skill_files(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        const std::string relative = fs::relative(it->path(), dir, ec).generic_string();
        if (!ec && is_safe_skill_path(relative))
            out.push_back(relative);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace agent
