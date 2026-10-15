
#include "project_instructions_plugin.h"

#include "agent/extensions.h"
#include "agent/prompt.h"
#include "agent/workspace.h"

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
constexpr std::size_t kMaxBytes = 8 * 1024;

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
    const std::string root = Workspace::root();
    return root.empty() ? fs::path() : fs::path(root) / "AGENTS.md";
}

std::string read_capped(const fs::path& path, bool& truncated) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    truncated = false;
    if (text.size() <= kMaxBytes)
        return text;
    // Cut at the last line boundary inside the cap: a marker mid-word reads as
    // corruption, and the agent should be able to trust what it was given.
    std::size_t cut = text.rfind('\n', kMaxBytes);
    if (cut == std::string::npos)
        cut = kMaxBytes;
    text.resize(cut);
    truncated = true;
    return text;
}

std::vector<std::string> nested_files(const fs::path& root) {
    std::vector<std::string> found;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return found;
    for (const auto& entry : it) {
        if (entry.is_directory(ec)) {
            // Depth by component count, from one computed relative path: two
            // temporaries would compare iterators from different objects.
            const fs::path rel = entry.path().lexically_relative(root);
            const int depth = static_cast<int>(std::distance(rel.begin(), rel.end()));
            if (!is_scannable_dir(entry.path()) || depth > kMaxDepth)
                it.disable_recursion_pending();
            continue;
        }
        if (entry.path().filename() != "AGENTS.md")
            continue;
        if (entry.path() == root / "AGENTS.md")
            continue;
        found.push_back(entry.path().lexically_relative(root).string());
        if (static_cast<int>(found.size()) >= kMaxNested) {
            it.disable_recursion_pending();
            break;
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::string render_block(const std::string& body, bool truncated,
                         const std::vector<std::string>& nested) {
    std::string out = "# Project instructions (AGENTS.md)\n\n";
    out += "This project states its own conventions. They are durable policy: a direct "
           "instruction from the user outranks them, and they outrank this system prompt "
           "where the two disagree.\n\n";
    out += body;
    if (!out.empty() && out.back() != '\n')
        out += '\n';
    if (truncated)
        out += "\n[truncated at 8 KB — read AGENTS.md for the rest]\n";
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
    bool truncated = false;
    const std::string body = read_capped(file, truncated);
    if (body.find_first_not_of(" \t\r\n") == std::string::npos)
        return block_; // whitespace only is not an instruction
    block_ = render_block(body, truncated, nested_files(file.parent_path()));
    return block_;
}

std::vector<std::string> ProjectInstructionsPlugin::report_lines() const {
    std::vector<std::string> lines;
    const fs::path file = root_file();
    if (file.empty() || !fs::exists(file)) {
        lines.push_back("(no AGENTS.md in this workspace)");
        return lines;
    }
    bool truncated = false;
    const std::string body = read_capped(file, truncated);
    std::ostringstream head;
    head << file.string() << " (" << body.size() << " bytes" << (truncated ? ", truncated" : "")
         << ")";
    lines.push_back(head.str());
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
