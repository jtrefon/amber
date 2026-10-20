
#include "project_instructions_plugin.h"

#include "agent/extensions.h"
#include "agent/prompt.h"
#include "agent/workspace.h"
#include "agent/worktree.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agent::plugins {

namespace {

namespace fs = std::filesystem;

// The block rides in the system prompt on every turn, so its size is a real
// cost. 8 KB is a generous instruction file; past that the tail is elided with
// a marker that says how to read the rest.
constexpr std::size_t kMaxBytes = std::size_t{8} * 1024;

// A nested AGENTS.md (a monorepo package's own rules) is *named* rather than
// inlined: the convention is that the nearest file wins, and the agent reads
// the one that applies when it works in that tree. The scan is bounded so a
// large checkout cannot make startup pay for it.
constexpr int kMaxNested = 12;
constexpr int kMaxDepth = 4;

bool is_scannable_dir(const fs::path& p) {
    const std::string name = p.filename().string();
    if (name.empty() || name[0] == '.')
        return false;
    return name != "node_modules" && name != "build" && name != "target" && name != "dist";
}

fs::path root_file() {
    const std::string root = Workspace::workspace_root();
    return root.empty() ? fs::path() : fs::path(root) / "AGENTS.md";
}

struct CappedText {
    std::string body;
    std::string digest;
    bool truncated = false;
};

bool blank_line(const std::string& line) {
    return line.find_first_not_of(" \t\r") == std::string::npos;
}

// A table of contents for the part past the cut: every top-level `##` section
// whose heading line was not kept, with its file line range. The range ends at
// the line before the next top-level heading, trailing blank lines trimmed, so
// the heading and the range come from the same scan and cannot drift.
std::string omitted_sections(const std::string& text, std::size_t first_omitted_line) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty())
        lines.push_back(line);

    struct Heading {
        std::size_t index = 0; // 0-based
        std::string title;
    };
    std::vector<Heading> headings;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].rfind("## ", 0) == 0)
            headings.push_back({i, lines[i].substr(3)});

    std::string out;
    for (std::size_t h = 0; h < headings.size(); ++h) {
        const std::size_t start = headings[h].index + 1; // 1-based
        if (start < first_omitted_line)
            continue;
        std::size_t end = (h + 1 < headings.size()) ? headings[h + 1].index : lines.size();
        while (end > start && blank_line(lines[end - 1]))
            --end;
        out += "- " + headings[h].title + " (lines " + std::to_string(start) + "-" +
               std::to_string(end) + ")\n";
    }
    return out;
}

CappedText read_capped(const fs::path& path) {
    CappedText out;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return out;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    if (text.size() <= kMaxBytes) {
        out.body = text;
        return out;
    }
    // Cut at the last line boundary inside the cap: a marker mid-word reads as
    // corruption, and the agent should be able to trust what it was given.
    std::size_t cut = text.rfind('\n', kMaxBytes);
    if (cut == std::string::npos)
        cut = kMaxBytes;
    out.body = text.substr(0, cut);
    out.truncated = true;
    const std::size_t kept_lines =
        static_cast<std::size_t>(std::count(out.body.begin(), out.body.end(), '\n')) + 1;
    out.digest = omitted_sections(text, kept_lines + 1);
    return out;
}

// A path under a directory that carries `.git` belongs to another tree (a
// nested repository or a linked worktree). Git usually refuses to descend
// there; this also catches a worktree whose metadata points elsewhere and the
// git implementations that do list it.
bool under_nested_repository(const fs::path& root, const fs::path& rel) {
    fs::path dir = root;
    for (const auto& part : rel.parent_path()) {
        dir /= part;
        std::error_code ec;
        if (fs::exists(dir / ".git", ec))
            return true;
    }
    return false;
}

// In a checkout, git answers both questions at once: ignore rules are honoured
// (tracked plus untracked-not-ignored), and git does not descend into a nested
// repository or linked worktree -- so a sibling copy's stale AGENTS.md never
// reaches the prompt.
std::vector<std::string> nested_files_git(const fs::path& root) {
    std::vector<std::string> found;
    for (const auto& rel : git_files_matching(root.string(), ":(glob)**/AGENTS.md")) {
        if (rel == "AGENTS.md" || under_nested_repository(root, fs::path(rel)))
            continue;
        found.push_back(rel);
        if (static_cast<int>(found.size()) >= kMaxNested)
            break;
    }
    return found;
}

// A tree without git: the walk, with the same boundaries. A directory carrying
// `.git` is a different tree, whatever its ignore rules are.
std::vector<std::string> nested_files_walk(const fs::path& root) {
    std::vector<std::string> found;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return found;
    for (const auto& entry : it) {
        // stat, not entry.is_directory(): a filesystem may report DT_UNKNOWN,
        // and a directory misread as a file would be descended into without the
        // boundary check below.
        if (fs::is_directory(entry.path(), ec)) {
            // Depth by component count, from one computed relative path: two
            // temporaries would compare iterators from different objects.
            const fs::path rel = entry.path().lexically_relative(root);
            const int depth = static_cast<int>(std::distance(rel.begin(), rel.end()));
            if (!is_scannable_dir(entry.path()) || depth > kMaxDepth ||
                fs::exists(entry.path() / ".git", ec))
                it.disable_recursion_pending();
            continue;
        }
        if (entry.path().filename() != "AGENTS.md" || entry.path() == root / "AGENTS.md")
            continue;
        const fs::path rel = entry.path().lexically_relative(root);
        if (under_nested_repository(root, rel))
            continue;
        found.push_back(rel.string());
        if (static_cast<int>(found.size()) >= kMaxNested) {
            it.disable_recursion_pending();
            break;
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<std::string> nested_files(const fs::path& root) {
    if (is_git_repo(root.string()))
        return nested_files_git(root);
    return nested_files_walk(root);
}

std::string render_block(const CappedText& text, const std::vector<std::string>& nested) {
    std::string out = "# Project instructions (AGENTS.md)\n\n";
    out += "This project states its own conventions. They are durable policy: a direct "
           "instruction from the user outranks them, and they outrank this system prompt "
           "where the two disagree.\n\n";
    out += text.body;
    if (!out.empty() && out.back() != '\n')
        out += '\n';
    if (text.truncated) {
        out += "\n[truncated at 8 KB — read AGENTS.md for the rest]\n";
        if (!text.digest.empty()) {
            out += "\nOmitted top-level sections (line ranges):\n";
            out += text.digest;
        }
    }
    if (!nested.empty()) {
        out += "\n## Nested instruction files\n\n";
        out += "The nearest file to the work wins. Read the one for the tree you are in:\n";
        for (const auto& path : nested)
            out += "- `" + path + "`\n";
    }
    return out;
}

} // namespace

void ProjectInstructionsPlugin::shutdown() {
    block_.clear();
    loaded_ = false;
}

std::string ProjectInstructionsPlugin::load_once() {
    if (loaded_)
        return block_;
    loaded_ = true;
    const fs::path file = root_file();
    if (file.empty() || !fs::exists(file))
        return block_;
    const CappedText text = read_capped(file);
    if (text.body.find_first_not_of(" \t\r\n") == std::string::npos)
        return block_; // whitespace only is not an instruction
    block_ = render_block(text, nested_files(file.parent_path()));
    return block_;
}

std::vector<std::string> ProjectInstructionsPlugin::report_lines() const {
    std::vector<std::string> lines;
    const fs::path file = root_file();
    if (file.empty() || !fs::exists(file)) {
        lines.emplace_back("(no AGENTS.md in this workspace)");
        return lines;
    }
    const CappedText text = read_capped(file);
    std::ostringstream head;
    head << file.string() << " (" << text.body.size() << " bytes"
         << (text.truncated ? ", truncated" : "") << ")";
    lines.emplace_back(head.str());
    for (const auto& path : nested_files(file.parent_path()))
        lines.push_back("nested: " + path);
    return lines;
}

std::vector<std::unique_ptr<Capability>> ProjectInstructionsPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // One whole capability: the plugin exists to put this text in the prompt,
    // and disabling it takes the text away with nothing else to unpick.
    caps.push_back(std::make_unique<PromptBlockCapability>(
        "project_instructions", prompt_priority::kProjectInstructions,
        [this] { return load_once(); }, PromptPlacement::System));
    return caps;
}

} // namespace agent::plugins
