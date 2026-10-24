#include "agent/artifacts.h"

#include "agent/workspace.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#include <vector>

namespace agent {

namespace {

namespace fs = std::filesystem;

struct Entry {
    fs::path path;
    std::uintmax_t size = 0;
    fs::file_time_type mtime;
};

std::vector<Entry> list_artifacts(const fs::path& dir) {
    std::vector<Entry> out;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(dir, ec)) {
        if (ec || !item.is_regular_file())
            continue;
        Entry entry;
        entry.path = item.path();
        entry.size = item.file_size(ec);
        entry.mtime = fs::last_write_time(item, ec);
        if (!ec)
            out.push_back(entry);
    }
    return out;
}

std::size_t total_bytes(const std::vector<Entry>& entries) {
    std::size_t total = 0;
    for (const auto& entry : entries)
        total += entry.size;
    return total;
}

// Oldest first, until the incoming write fits both the file and byte budgets.
void prune(const fs::path& dir, std::size_t incoming) {
    std::vector<Entry> entries = list_artifacts(dir);
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
    std::size_t total = total_bytes(entries);
    std::size_t index = 0;
    while (index < entries.size() &&
           (entries.size() - index >= kArtifactMaxFiles || total + incoming > kArtifactMaxBytes)) {
        std::error_code ec;
        total -= entries[index].size;
        fs::remove(entries[index].path, ec);
        ++index;
    }
}

std::string random_id() {
    std::random_device device;
    std::mt19937_64 generator(device());
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(generator()));
    return buffer;
}

std::optional<std::string> read_range(const fs::path& path, std::size_t offset, std::size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    in.seekg(static_cast<std::streamoff>(offset));
    std::string out(limit, '\0');
    in.read(out.data(), static_cast<std::streamsize>(limit));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

// A single line can be megabytes (minified JSON, logs). Keep a window around
// the match so one hit does not refill the context.
std::string window_around(const std::string& line, const std::string& needle) {
    constexpr std::size_t kWindow = 240;
    constexpr std::size_t kBefore = 80;
    if (line.size() <= kWindow)
        return line;
    const size_t at = line.find(needle);
    const size_t from = (at == std::string::npos || at <= kBefore) ? 0 : at - kBefore;
    const size_t length = std::min(kWindow, line.size() - from);
    std::string out = (from > 0 ? "\xe2\x80\xa6" : "") + line.substr(from, length);
    if (from + length < line.size())
        out += "\xe2\x80\xa6";
    return out;
}

std::optional<std::string> grep_lines(const fs::path& path, const std::string& needle,
                                      std::size_t limit, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read artifact";
        return std::nullopt;
    }
    std::string out;
    std::string line;
    std::size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.find(needle) == std::string::npos)
            continue;
        const std::string hit = std::to_string(number) + ":" + window_around(line, needle) + "\n";
        if (out.size() + hit.size() > limit)
            break;
        out += hit;
    }
    if (out.empty()) {
        error = "no matches for '" + needle + "'";
        return std::nullopt;
    }
    return out;
}

// Validate the handle and resolve it inside the artifact directory; the id is
// the only input, so no caller-supplied path can escape.
std::optional<fs::path> artifact_path(const std::string& id, std::string& error) {
    if (!valid_artifact_id(id)) {
        error = "invalid artifact id";
        return std::nullopt;
    }
    fs::path path = fs::path(artifact_dir()) / id;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = "unknown or expired artifact " + id;
        return std::nullopt;
    }
    return path;
}

std::optional<std::string> read_byte_range(const fs::path& path, std::size_t offset,
                                           std::size_t limit, std::string& error) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        error = "cannot stat artifact";
        return std::nullopt;
    }
    if (offset >= size) {
        error =
            "offset " + std::to_string(offset) + " beyond artifact size " + std::to_string(size);
        return std::nullopt;
    }
    const std::size_t want = std::min(limit, static_cast<std::size_t>(size - offset));
    auto out = read_range(path, offset, want);
    if (!out) {
        error = "cannot read artifact";
        return std::nullopt;
    }
    return out;
}

} // namespace

std::string artifact_dir() {
    return Workspace::state_dir() + "/artifacts";
}

bool valid_artifact_id(const std::string& id) {
    return id.size() == 16 &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string store_tool_artifact(const std::string& payload) {
    if (payload.size() < kArtifactMinBytes || payload.size() > kArtifactMaxBytes)
        return {};
    const fs::path dir(artifact_dir());
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir))
        return {};
    prune(dir, payload.size());
    std::string id = random_id();
    const fs::path temp = dir / (id + ".tmp");
    {
        // The directory is the user's own XDG state root: Workspace::state_dir()
        // rejects a non-absolute or ".." root, and the file name is a generated
        // 16-hex id. This is the same trust boundary as every other state write.
        std::ofstream out(temp, std::ios::binary | std::ios::trunc); // lgtm[cpp/path-injection]
        if (!out)
            return {};
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!out) {
            fs::remove(temp, ec);
            return {};
        }
    }
    fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                    ec);
    fs::rename(temp, dir / id, ec);
    if (ec) {
        fs::remove(temp, ec);
        return {};
    }
    return id;
}

std::optional<std::string> read_tool_artifact(const std::string& id, std::size_t offset,
                                              std::size_t limit, const std::string& grep,
                                              std::string& error) {
    error.clear();
    auto path = artifact_path(id, error);
    if (!path)
        return std::nullopt;
    if (limit == 0)
        limit = kArtifactReadDefault;
    limit = std::min(limit, kArtifactReadMax);
    if (grep.empty())
        return read_byte_range(*path, offset, limit, error);
    auto hits = grep_lines(*path, grep, limit, error);
    if (!hits)
        error += " in artifact " + id;
    return hits;
}

} // namespace agent
