
#include "agent/skill_file.h"
#include "agent/config.h"
#include "agent/workspace.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

void set_field(agent::SkillMeta& meta, const std::string& key, const std::string& val) {
    if (key == "name")
        meta.name = val;
    else if (key == "description")
        meta.description = val;
    else if (key == "license")
        meta.license = val;
    else if (key == "compatibility")
        meta.compatibility = val;
}

// Parse an inline YAML flow map `{k: v, k2: v2}` into a JSON object.
void merge_flow_map(json& dst, const std::string& val) {
    std::string inner = trim(val);
    if (inner.size() >= 2 && inner.front() == '{' && inner.back() == '}') {
        inner = inner.substr(1, inner.size() - 2);
    }
    std::stringstream ss(inner);
    std::string part;
    while (std::getline(ss, part, ',')) {
        size_t colon = part.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = trim(part.substr(0, colon));
        std::string value = trim(part.substr(colon + 1));
        if (key.empty())
            continue;
        dst[key] = value;
    }
}

// Join an accumulated folded block (`>` continuation lines) into `meta`.
void flush_folded(agent::SkillMeta& meta, const std::string& key, std::vector<std::string>& lines) {
    std::string joined;
    for (const std::string& l : lines) {
        if (!joined.empty())
            joined += " ";
        joined += l;
    }
    set_field(meta, key, joined);
    lines.clear();
}

} // namespace

namespace agent {

bool is_kebab_name(const std::string& name) noexcept {
    auto valid = [](char c) {
        bool lower = c >= 'a' && c <= 'z';
        bool digit = c >= '0' && c <= '9';
        return lower || digit || c == '-';
    };
    return !name.empty() && std::all_of(name.begin(), name.end(), valid);
}

// The frontmatter parser's mutable state.
struct FrontmatterState {
    bool in_frontmatter = true;
    bool in_metadata = false;
    std::string folded_key;
    std::vector<std::string> folded_lines;
};

// The "key: value" forms: the metadata namespace, a folded block, or a plain
// field.
void apply_frontmatter_field(const std::string& key, const std::string& val, SkillMeta& meta,
                             FrontmatterState& st) {
    if (key == "metadata") {
        if (val.empty())
            st.in_metadata = true;
        else
            merge_flow_map(meta.metadata, val);
        return;
    }
    if (val == ">") {
        st.folded_key = key;
        st.folded_lines.clear();
        return;
    }
    if (key == "name" || key == "description" || key == "license" || key == "compatibility")
        set_field(meta, key, val);
}

// One frontmatter line. Returns false at the closing "---".
bool parse_frontmatter_line(const std::string& line, SkillMeta& meta, FrontmatterState& st) {
    if (trim(line) == "---")
        return false;
    const bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
    if (st.in_metadata) {
        if (indented) {
            const size_t c = line.find(':');
            if (c != std::string::npos)
                meta.metadata[trim(line.substr(0, c))] = trim(line.substr(c + 1));
            return true;
        }
        st.in_metadata = false;
    }
    if (!st.folded_key.empty()) {
        if (indented) {
            st.folded_lines.push_back(trim(line));
            return true;
        }
        flush_folded(meta, st.folded_key, st.folded_lines);
        st.folded_key.clear();
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos)
        return true;
    apply_frontmatter_field(trim(line.substr(0, colon)), trim(line.substr(colon + 1)), meta, st);
    return true;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i)
            out += "\n";
        out += lines[i];
    }
    return out;
}

std::optional<SkillMeta> parse_skill_meta(const std::string& contents) {
    std::istringstream in(contents);
    std::string line;

    while (std::getline(in, line)) {
        if (trim(line).empty())
            continue;
        break;
    }
    if (trim(line) != "---")
        return std::nullopt;

    SkillMeta meta;
    std::vector<std::string> body_lines;
    FrontmatterState st;

    while (std::getline(in, line)) {
        if (!st.in_frontmatter) {
            body_lines.push_back(line);
            continue;
        }
        if (!parse_frontmatter_line(line, meta, st))
            st.in_frontmatter = false;
    }
    if (!st.folded_key.empty())
        flush_folded(meta, st.folded_key, st.folded_lines);

    meta.body = join_lines(body_lines);
    if (meta.name.empty() && meta.description.empty() && meta.body.empty())
        return std::nullopt;
    return meta;
}

void warn(std::vector<std::string>* warnings, const std::string& message) {
    if (warnings)
        warnings->push_back(message);
}

// Load one skill directory: name validation, then SKILL.md presence and parse.
std::optional<SkillFile> load_skill_dir(const fs::path& dir, const std::string& name,
                                        SkillScope scope, std::vector<std::string>* warnings) {
    if (!is_kebab_name(name)) {
        warn(warnings, "skill '" + name + "': invalid name (lowercase letters, digits, '-' only)");
        return std::nullopt;
    }
    const fs::path sk_path = dir / "SKILL.md";
    std::error_code ec;
    if (!fs::exists(sk_path, ec)) {
        warn(warnings, "skill '" + name + "': no SKILL.md, skipping");
        return std::nullopt;
    }
    std::ifstream f(sk_path);
    if (!f.is_open()) {
        warn(warnings, "skill '" + name + "': unreadable SKILL.md, skipping");
        return std::nullopt;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    auto meta = parse_skill_meta(ss.str());
    if (!meta) {
        warn(warnings, "skill '" + name + "': malformed SKILL.md, skipping");
        return std::nullopt;
    }
    meta->name = name;
    meta->description = trim(meta->description);
    return SkillFile{name, dir.string(), scope, std::move(*meta)};
}

std::vector<SkillFile> scan_skill_dir(const std::string& root, SkillScope scope,
                                      std::vector<std::string>* warnings) {
    std::vector<SkillFile> out;
    if (root.empty())
        return out;
    std::error_code ec;
    if (!fs::exists(root, ec))
        return out;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec)
            break;
        std::error_code e2;
        if (!entry.is_directory(e2))
            continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.')
            continue;
        if (auto skill = load_skill_dir(entry.path(), name, scope, warnings))
            out.push_back(std::move(*skill));
    }
    std::sort(out.begin(), out.end(),
              [](const SkillFile& a, const SkillFile& b) { return a.name < b.name; });
    return out;
}

std::vector<SkillFile> scan_skills(const SkillScanPaths& paths, bool interop_enabled,
                                   std::vector<std::string>* warnings) {
    std::vector<SkillFile> result;
    std::set<std::string> seen;
    auto absorb = [&](std::vector<SkillFile> files) {
        for (SkillFile& f : files) {
            if (seen.insert(f.name).second)
                result.push_back(std::move(f));
        }
    };
    absorb(scan_skill_dir(paths.project, SkillScope::Project, warnings));
    absorb(scan_skill_dir(paths.global, SkillScope::Global, warnings));
    if (interop_enabled) {
        absorb(scan_skill_dir(paths.claude, SkillScope::Interop, warnings));
        absorb(scan_skill_dir(paths.codex, SkillScope::Interop, warnings));
    }
    return result;
}

SkillScanPaths default_scan_paths() {
    SkillScanPaths paths;
    paths.project = Workspace::local_dir() + "/skills";
    paths.global = global_config_dir() + "/skills";
    std::string ws = Workspace::root();
    paths.claude = ws + "/.claude/skills";
    paths.codex = ws + "/.codex/skills";
    return paths;
}

} // namespace agent
