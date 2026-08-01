
#include "agent/skill_install.h"

#include "agent/archive_util.h"
#include "agent/skill_file.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace agent {

namespace {

// Find the skill name: the single top-level directory containing SKILL.md.
// Returns "" when the archive layout is not a skill pack.
std::string pack_skill_name(const std::string& listing) {
    std::istringstream ss(listing);
    std::string entry;
    while (std::getline(ss, entry)) {
        if (entry.size() < 9) continue;
        if (entry.compare(entry.size() - 9, 9, "/SKILL.md") != 0) continue;
        size_t first = entry.find_first_not_of("./");
        size_t slash = entry.find('/', first);
        if (first == std::string::npos || slash == std::string::npos)
            return "";
        return entry.substr(first, slash - first);
    }
    return "";
}

} // namespace

std::string install_skill_pack(const std::string& source,
                               const std::string& dest_root) {
    std::string err;
    std::string bytes = fetch_bytes(source, err);
    if (bytes.empty()) return err;

    std::string tmp = "/tmp/amber-skill-install-" + std::to_string(getpid());
    std::string tgz = tmp + ".tar.gz";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    {
        std::ofstream f(tgz, std::ios::binary);
        f << bytes;
    }
    std::string listing = list_tar_gz(tgz);
    std::string name = pack_skill_name(listing);
    if (name.empty()) {
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::remove(tgz, ec);
        return "archive is not a skill pack (SKILL.md must sit inside one "
               "top-level directory)";
    }
    if (!is_kebab_name(name)) {
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::remove(tgz, ec);
        return "invalid skill name '" + name + "'";
    }
    if (!unpack_tar_gz(tgz, tmp).empty()) {
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::remove(tgz, ec);
        return "cannot unpack archive";
    }
    std::string body_path = tmp + "/" + name + "/SKILL.md";
    std::ifstream body_in(body_path, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(body_in)),
                     std::istreambuf_iterator<char>());
    if (!parse_skill_meta(body)) {
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::remove(tgz, ec);
        return "malformed SKILL.md (missing or invalid frontmatter)";
    }
    std::filesystem::create_directories(dest_root, ec);
    std::string dest = dest_root + "/" + name;
    std::filesystem::remove_all(dest, ec);
    std::error_code ec2;
    std::filesystem::copy(tmp + "/" + name, dest,
                          std::filesystem::copy_options::recursive, ec2);
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::remove(tgz, ec);
    if (ec2) return "cannot stage skill: " + ec2.message();
    return "";
}

std::string uninstall_skill(const std::string& name,
                            const std::string& dest_root) {
    if (!is_kebab_name(name)) return "invalid skill name: " + name;
    std::string dest = dest_root + "/" + name;
    if (!std::filesystem::exists(dest)) return "skill not installed: " + name;
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
    if (ec) return "cannot remove " + dest;
    return "";
}

} // namespace agent
