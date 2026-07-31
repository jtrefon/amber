
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
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

void set_field(agent::SkillMeta& meta,
               const std::string& key,
               const std::string& val) {
    if (key == "name") meta.name = val;
    else if (key == "description") meta.description = val;
    else if (key == "license") meta.license = val;
    else if (key == "compatibility") meta.compatibility = val;
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
        if (colon == std::string::npos) continue;
        std::string key = trim(part.substr(0, colon));
        std::string value = trim(part.substr(colon + 1));
        if (key.empty()) continue;
        dst[key] = value;
    }
}

// Join an accumulated folded block (`>` continuation lines) into `meta`.
void flush_folded(agent::SkillMeta& meta,
                  const std::string& key,
                  std::vector<std::string>& lines) {
    std::string joined;
    for (const std::string& l : lines) {
        if (!joined.empty()) joined += " ";
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

std::optional<SkillMeta> parse_skill_meta(const std::string& contents) {
    std::istringstream in(contents);
    std::string line;

    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        break;
    }
    if (trim(line) != "---") return std::nullopt;

    SkillMeta meta;
    std::vector<std::string> body_lines;
    bool in_frontmatter = true;
    std::string folded_key;
    std::vector<std::string> folded_lines;
    bool in_metadata = false;

    while (std::getline(in, line)) {
        if (!in_frontmatter) {
            body_lines.push_back(line);
            continue;
        }
        if (trim(line) == "---") {
            in_frontmatter = false;
            continue;
        }
        bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        if (in_metadata) {
            if (indented) {
                size_t c = line.find(':');
                if (c != std::string::npos)
                    meta.metadata[trim(line.substr(0, c))] =
                        trim(line.substr(c + 1));
                continue;
            }
            in_metadata = false;
        }
        if (!folded_key.empty()) {
            if (indented) {
                folded_lines.push_back(trim(line));
                continue;
            }
            flush_folded(meta, folded_key, folded_lines);
            folded_key.clear();
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));

        if (key == "metadata") {
            if (val.empty()) {
                in_metadata = true;
            } else {
                merge_flow_map(meta.metadata, val);
            }
        } else if (val == ">") {
            folded_key = key;
            folded_lines.clear();
        } else if (key == "name" || key == "description" ||
                   key == "license" || key == "compatibility") {
            set_field(meta, key, val);
        }
    }
    if (!folded_key.empty()) flush_folded(meta, folded_key, folded_lines);

    std::string body;
    for (size_t i = 0; i < body_lines.size(); ++i) {
        if (i) body += "\n";
        body += body_lines[i];
    }
    meta.body = body;
    if (meta.name.empty() && meta.description.empty() && meta.body.empty())
        return std::nullopt;
    return meta;
}

std::vector<SkillFile> scan_skill_dir(const std::string& root,
                                      SkillScope scope,
                                      std::vector<std::string>* warnings) {
    std::vector<SkillFile> out;
    if (root.empty()) return out;
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        std::error_code e2;
        if (!entry.is_directory(e2)) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (!is_kebab_name(name)) {
            if (warnings) {
                warnings->push_back("skill '" + name + "': invalid name "
                                    "(lowercase letters, digits, '-' only)");
            }
            continue;
        }
        fs::path sk_path = entry.path() / "SKILL.md";
        if (!fs::exists(sk_path, e2)) {
            if (warnings)
                warnings->push_back("skill '" + name +
                                    "': no SKILL.md, skipping");
            continue;
        }
        std::ifstream f(sk_path);
        if (!f.is_open()) {
            if (warnings)
                warnings->push_back("skill '" + name +
                                    "': unreadable SKILL.md, skipping");
            continue;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        auto meta = parse_skill_meta(ss.str());
        if (!meta) {
            if (warnings)
                warnings->push_back("skill '" + name +
                                    "': malformed SKILL.md, skipping");
            continue;
        }
        meta->name = name;
        meta->description = trim(meta->description);
        out.push_back(
            SkillFile{name, entry.path().string(), scope, std::move(*meta)});
    }
    std::sort(out.begin(), out.end(),
              [](const SkillFile& a, const SkillFile& b) {
                  return a.name < b.name;
              });
    return out;
}

std::vector<SkillFile> scan_skills(const SkillScanPaths& paths,
                                   bool interop_enabled,
                                   std::vector<std::string>* warnings) {
    std::vector<SkillFile> result;
    std::set<std::string> seen;
    auto absorb = [&](std::vector<SkillFile> files) {
        for (SkillFile& f : files) {
            if (seen.insert(f.name).second) result.push_back(std::move(f));
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
