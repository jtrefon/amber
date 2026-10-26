#include "agent/skill_cli.h"

#include "agent/github_index.h"
#include "agent/skill_catalog.h"
#include "agent/skill_file.h"
#include "agent/skill_install.h"
#include "agent/workspace.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <nlohmann/json.hpp>

namespace agent {

namespace {

namespace fs = std::filesystem;

using json = nlohmann::json;

const char* kUsage = "usage: amber-cli skills <verb> [args]\n"
                     "  search <query> [--limit N] [--json]\n"
                     "  list [--global] [--json]\n"
                     "  show <name|owner/repo/skill> [--json]\n"
                     "  install <id|owner/repo|url> [--global] [--yes] [--json]\n"
                     "  update [<name>|--all] [--check] [--json]\n"
                     "  remove <name> [--global] [--json]\n"
                     "  sources [--refresh] [--json]\n";

SkillVerb verb_from(const std::string& word) {
    if (word == "search")
        return SkillVerb::Search;
    if (word == "list")
        return SkillVerb::List;
    if (word == "show")
        return SkillVerb::Show;
    if (word == "install")
        return SkillVerb::Install;
    if (word == "update")
        return SkillVerb::Update;
    if (word == "remove")
        return SkillVerb::Remove;
    if (word == "sources")
        return SkillVerb::Sources;
    return SkillVerb::None;
}

std::string scope_root(const std::string& scope) {
    if (scope == "global")
        return global_config_dir() + "/skills";
    return Workspace::local_dir() + "/skills";
}

std::string local_hash(const std::string& root, const std::string& name) {
    std::ifstream f(root + "/" + name + "/SKILL.md", std::ios::binary);
    if (!f)
        return {};
    const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return skill_content_hash(body);
}

// "owner/repo" or "owner/repo/skill" -> the registry identity.
bool split_target(const std::string& target, SkillRef& ref, std::string& error) {
    std::vector<std::string> parts;
    std::string part;
    for (char ch : target) {
        if (ch == '/') {
            parts.push_back(part);
            part.clear();
        } else {
            part += ch;
        }
    }
    parts.push_back(part);
    if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) {
        error = "expected owner/repo or owner/repo/skill";
        return false;
    }
    ref.source = parts[0] + "/" + parts[1];
    ref.name = parts.size() > (size_t)2 ? parts.back() : parts[1];
    ref.id = ref.source + "/" + ref.name;
    return true;
}

bool looks_like_pack(const std::string& target) {
    return target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0 ||
           (target.size() > 7 && target.compare(target.size() - 7, 7, ".tar.gz") == 0) ||
           (target.size() > 4 && target.compare(target.size() - 4, 4, ".tgz") == 0);
}

std::string state_for(const std::vector<SkillEntry>& entries, const std::string& name) {
    for (const auto& entry : entries) {
        if (entry.name == name && !entry.state.empty())
            return entry.state;
    }
    return "enabled";
}

std::vector<InstalledPackage> packages_from(const std::string& scope,
                                            const std::vector<SkillEntry>& entries) {
    std::vector<InstalledPackage> packages;
    SkillLedger ledger = SkillLedger::for_root(scope_root(scope));
    ledger.load();
    for (const auto& [name, entry] : ledger.entries()) {
        InstalledPackage package;
        package.name = name;
        package.scope = scope;
        package.id = entry.id;
        package.source = entry.source;
        package.ref = entry.ref;
        package.hash = entry.hash;
        package.local_hash = entry.local_hash;
        package.files = entry.files;
        package.bytes = entry.bytes;
        package.enabled = entry.enabled;
        package.state = state_for(entries, name);
        packages.push_back(std::move(package));
    }
    return packages;
}

std::string cache_summary(long long& bytes_out) {
    const std::string root = skills_index_cache_dir();
    long long bytes = 0;
    int files = 0;
    std::error_code ec;
    for (const auto& it : fs::recursive_directory_iterator(root, ec)) {
        if (ec)
            break;
        if (it.is_regular_file(ec)) {
            ++files;
            bytes += static_cast<long long>(it.file_size(ec));
        }
    }
    bytes_out = bytes;
    return std::to_string(files) + " files, " + std::to_string(bytes) + " bytes";
}

SkillIndexOptions index_options() {
    SkillIndexOptions options;
    options.cache_root = skills_index_cache_dir();
    if (const char* base = std::getenv("AMBER_SKILLS_INDEX_URL"))
        options.base_url = base;
    if (const char* token = std::getenv("SKILLS_SH_TOKEN"))
        options.token = token;
    return options;
}

GithubOptions github_options() {
    GithubOptions options;
    if (const char* api = std::getenv("AMBER_GITHUB_API_URL"))
        options.api_base = api;
    if (const char* raw = std::getenv("AMBER_GITHUB_RAW_URL"))
        options.raw_base = raw;
    if (const char* token = std::getenv("SKILLS_SH_TOKEN"))
        options.token = token;
    return options;
}

int run_search(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err) {
    SkillsShIndex index(index_options());
    const IndexResult result = index.search(cmd.target, cmd.limit, CancellationToken{});
    if (!result.page) {
        err << "skills search: " << result.error.detail << "\n";
        return 1;
    }
    const SkillPage& page = *result.page;
    out << (cmd.json ? skill_search_json(page) : skill_search_text(page));
    return 0;
}

int run_list(const SkillCliCommand& cmd, std::ostream& out) {
    Config cfg;
    SkillCatalog catalog(cfg);
    const auto entries = catalog.entries();
    std::vector<InstalledPackage> packages;
    if (!cmd.global) {
        const auto project = packages_from("project", entries);
        packages.insert(packages.end(), project.begin(), project.end());
    }
    const auto global = packages_from("global", entries);
    packages.insert(packages.end(), global.begin(), global.end());
    out << (cmd.json ? skill_list_json(packages) : skill_list_text(packages));
    return 0;
}

int run_show(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err) {
    Config cfg;
    SkillCatalog catalog(cfg);
    const auto entries = catalog.entries();
    std::vector<InstalledPackage> packages = packages_from("project", entries);
    const auto global_packages = packages_from("global", entries);
    packages.insert(packages.end(), global_packages.begin(), global_packages.end());
    for (const auto& package : packages) {
        if (package.name != cmd.target)
            continue;
        out << (cmd.json ? skill_list_json({package}) : skill_list_text({package}));
        return 0;
    }

    SkillRef ref;
    std::string error;
    if (!split_target(cmd.target, ref, error)) {
        err << "skills show: no installed skill named '" << cmd.target << "'\n";
        return 1;
    }
    GitHubIndex github(github_options());
    const DetailResult detail = github.detail(ref, CancellationToken{});
    if (!detail.detail) {
        err << "skills show: " << detail.error.detail << "\n";
        return 1;
    }
    const SkillDetail& found = *detail.detail;
    out << (cmd.json ? skill_detail_json(found) : skill_detail_text(found));
    return 0;
}

int install_pack(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err) {
    const std::string root = scope_root(cmd.global ? "global" : "project");
    const std::string error = install_skill_pack(cmd.target, root);
    if (!error.empty()) {
        err << "skills install: " << error << "\n";
        return 1;
    }
    out << "installed " << cmd.target << " into " << root << "\n";
    return 0;
}

int confirm_unattended(const SkillCliCommand& cmd, const SkillRef& ref, std::ostream& err) {
    if (cmd.yes)
        return 0;
    // Headless installs are unattended: the review gate becomes an explicit
    // flag, never a silent default.
    err << "skills install: refusing to install '" << ref.id
        << "' unattended (community source, no audit).\n"
        << "  read it with 'amber-cli skills show " << ref.id << "', then pass --yes to confirm.\n";
    return 1;
}

int write_install(const SkillCliCommand& cmd, const SkillRef& ref, const SkillDetail& detail,
                  std::ostream& out, std::ostream& err) {
    const AssetsResult assets =
        GitHubIndex(github_options()).fetch_assets(detail, CancellationToken{});
    if (!assets.files) {
        err << "skills install: " << assets.error.detail << "\n";
        return 1;
    }
    SkillInstallRequest request;
    request.name = skill_slug(ref.id);
    request.dest_root = scope_root(cmd.global ? "global" : "project");
    for (const auto& file : *assets.files)
        request.files.push_back({file.path, file.contents});
    const SkillInstallResult result = install_skill_files(request);
    if (!result.ok) {
        err << "skills install: " << result.error << "\n";
        return 1;
    }
    SkillLedger ledger = SkillLedger::for_root(request.dest_root);
    ledger.load();
    ledger.upsert(request.name,
                  ledger_entry_for(detail, request.name, request.files, result, true));
    if (cmd.json) {
        out << json{{"ok", true},
                    {"name", request.name},
                    {"id", ref.id},
                    {"root", request.dest_root},
                    {"files", result.files},
                    {"bytes", result.bytes}}
                   .dump(2)
            << "\n";
    } else {
        out << "installed " << request.name << " · " << result.files << " files · " << result.bytes
            << " bytes → " << request.dest_root << "\n";
    }
    return 0;
}

int install_registry(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err) {
    SkillRef ref;
    std::string error;
    if (!split_target(cmd.target, ref, error)) {
        err << "skills install: " << error << "\n";
        return 2;
    }
    GitHubIndex github(github_options());
    const DetailResult detail = github.detail(ref, CancellationToken{});
    if (!detail.detail) {
        err << "skills install: " << detail.error.detail << "\n";
        return 1;
    }
    if (const int rc = confirm_unattended(cmd, ref, err); rc != 0)
        return rc;
    return write_install(cmd, ref, *detail.detail, out, err);
}

struct UpdatePlan {
    std::string error;
    bool available = false;
    bool local_edits = false;
    SkillDetail detail;
};

SkillRef ref_of(const InstalledPackage& package) {
    SkillRef ref;
    ref.id = package.id;
    ref.source = package.source;
    ref.name = package.name;
    return ref;
}

UpdatePlan plan_update(GitHubIndex& github, const InstalledPackage& package) {
    UpdatePlan plan;
    const DetailResult detail = github.detail(ref_of(package), CancellationToken{});
    if (!detail.detail) {
        plan.error = detail.error.detail;
        return plan;
    }
    plan.detail = *detail.detail;
    if (plan.detail.hash == package.hash)
        return plan;
    plan.available = true;
    plan.local_edits = local_hash(scope_root(package.scope), package.name) != package.local_hash;
    return plan;
}

std::string apply_plan(const InstalledPackage& package, const SkillDetail& detail) {
    const AssetsResult assets =
        GitHubIndex(github_options()).fetch_assets(detail, CancellationToken{});
    if (!assets.files)
        return "error: " + assets.error.detail;
    SkillInstallRequest request;
    request.name = package.name;
    request.dest_root = scope_root(package.scope);
    for (const auto& file : *assets.files)
        request.files.push_back({file.path, file.contents});
    request.replace = true;
    const SkillInstallResult result = install_skill_files(request);
    if (!result.ok)
        return "error: " + result.error;
    SkillLedger ledger = SkillLedger::for_root(request.dest_root);
    ledger.load();
    ledger.upsert(package.name,
                  ledger_entry_for(detail, package.name, request.files, result, package.enabled));
    return "updated";
}

std::string update_status(const SkillCliCommand& cmd, const InstalledPackage& package,
                          const UpdatePlan& plan, int& failures) {
    if (!plan.error.empty()) {
        ++failures;
        return "error: " + plan.error;
    }
    if (plan.local_edits)
        return "locally modified (update refused)";
    if (!plan.available)
        return "up to date";
    if (cmd.check)
        return "update available";
    const std::string status = apply_plan(package, plan.detail);
    if (status.rfind("error:", 0) == 0)
        ++failures;
    return status;
}

int run_update(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err) {
    (void)err; // failures are reported per row, not as a single line
    Config cfg;
    SkillCatalog catalog(cfg);
    const auto entries = catalog.entries();
    std::vector<InstalledPackage> packages = packages_from("project", entries);
    const auto global_packages = packages_from("global", entries);
    packages.insert(packages.end(), global_packages.begin(), global_packages.end());

    GitHubIndex github(github_options());
    json report = json::array();
    int failures = 0;
    for (const auto& package : packages) {
        if (!cmd.all && !cmd.target.empty() && package.name != cmd.target)
            continue;
        if (cmd.global && package.scope != "global")
            continue;
        const UpdatePlan plan = plan_update(github, package);
        const std::string status = update_status(cmd, package, plan, failures);
        report.push_back({{"name", package.name},
                          {"scope", package.scope},
                          {"status", status},
                          {"from", package.hash},
                          {"to", plan.error.empty() ? plan.detail.hash : std::string()}});
    }
    if (cmd.json) {
        out << report.dump(2) << "\n";
    } else {
        for (const auto& row : report)
            out << row["scope"].get<std::string>() << " · " << row["name"].get<std::string>()
                << ": " << row["status"].get<std::string>() << "\n";
    }
    return failures > 0 ? 1 : 0;
}

int run_remove(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err) {
    if (cmd.target.empty()) {
        err << "skills remove: a skill name is required\n";
        return 2;
    }
    const std::string scope = cmd.global ? "global" : "project";
    const std::string root = scope_root(scope);
    const std::string error = uninstall_skill(cmd.target, root);
    if (!error.empty()) {
        err << "skills remove: " << error << "\n";
        return 1;
    }
    SkillLedger ledger = SkillLedger::for_root(root);
    ledger.load();
    ledger.remove(cmd.target);
    if (cmd.json)
        out << json{{"ok", true}, {"name", cmd.target}, {"scope", scope}}.dump(2) << "\n";
    else
        out << "removed " << cmd.target << " from " << root << "\n";
    return 0;
}

int run_sources(const SkillCliCommand& cmd, std::ostream& out) {
    const SkillSources sources = skill_sources(cmd.refresh);
    if (cmd.json) {
        out << json{{"index", sources.index_base},
                    {"github_api", sources.github_api},
                    {"github_raw", sources.github_raw},
                    {"cache", sources.cache_dir},
                    {"cache_summary", sources.cache_summary}}
                   .dump(2)
            << "\n";
        return 0;
    }
    out << "index       " << sources.index_base << " (unauthenticated search)\n";
    out << "github api  " << sources.github_api << "\n";
    out << "github raw  " << sources.github_raw << "\n";
    out << "cache       " << sources.cache_dir << " (" << sources.cache_summary << ")\n";
    return 0;
}

} // namespace

SkillSources skill_sources(bool refresh) {
    if (refresh) {
        std::error_code ec;
        fs::remove_all(skills_index_cache_dir(), ec);
    }
    long long bytes = 0;
    SkillSources sources;
    sources.index_base = index_options().base_url;
    sources.github_api = github_options().api_base;
    sources.github_raw = github_options().raw_base;
    sources.cache_dir = skills_index_cache_dir();
    sources.cache_summary = cache_summary(bytes);
    return sources;
}

bool parse_cli_flag(const std::string& arg, size_t& i, const std::vector<std::string>& args,
                    SkillCliCommand& cmd) {
    if (arg == "--json")
        cmd.json = true;
    else if (arg == "--global")
        cmd.global = true;
    else if (arg == "--yes" || arg == "-y")
        cmd.yes = true;
    else if (arg == "--all")
        cmd.all = true;
    else if (arg == "--check")
        cmd.check = true;
    else if (arg == "--refresh")
        cmd.refresh = true;
    else if (arg == "--limit" && i + 1 < args.size())
        cmd.limit = std::max(1, std::atoi(args[++i].c_str()));
    else if (arg.rfind("--", 0) == 0) {
        cmd.error = "unknown flag '" + arg + "'";
        return true;
    } else {
        return false;
    }
    return true;
}

SkillCliCommand parse_skill_cli(const std::vector<std::string>& args) {
    SkillCliCommand cmd;
    if (args.empty()) {
        cmd.error = "a verb is required";
        return cmd;
    }
    cmd.verb = verb_from(args[0]);
    if (cmd.verb == SkillVerb::None) {
        cmd.error = "unknown verb '" + args[0] + "'";
        return cmd;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        if (parse_cli_flag(args[i], i, args, cmd))
            continue;
        if (!cmd.error.empty())
            return cmd;
        if (!cmd.target.empty()) {
            cmd.error = "unexpected argument '" + args[i] + "'";
            return cmd;
        }
        cmd.target = args[i];
    }
    if ((cmd.verb == SkillVerb::Search || cmd.verb == SkillVerb::Install ||
         cmd.verb == SkillVerb::Show || cmd.verb == SkillVerb::Remove) &&
        cmd.target.empty())
        cmd.error = "a target is required";
    return cmd;
}

std::string skill_search_text(const SkillPage& page) {
    std::ostringstream out;
    for (const auto& skill : page.skills)
        out << skill.name << "\t" << skill.source << "\t" << skill.installs << "\n";
    out << page.skills.size() << " of " << page.total << " results"
        << (page.cached ? " (cached)" : "") << "\n";
    return out.str();
}

std::string skill_search_json(const SkillPage& page) {
    json skills = json::array();
    for (const auto& skill : page.skills)
        skills.push_back({{"id", skill.id},
                          {"name", skill.name},
                          {"source", skill.source},
                          {"installs", skill.installs}});
    return json{{"skills", skills},
                {"count", page.total},
                {"cached", page.cached},
                {"stale", page.stale}}
               .dump(2) +
           "\n";
}

std::string skill_detail_text(const SkillDetail& detail) {
    std::ostringstream out;
    out << detail.ref.name << " · " << detail.ref.source << " · " << detail.ref.installs
        << " installs\n";
    if (!detail.description.empty())
        out << detail.description << "\n";
    out << (detail.license.empty() ? "no license" : detail.license) << " · " << detail.assets.size()
        << " files · ~" << detail.body.size() / 4 << " tokens · not audited\n";
    return out.str();
}

std::string skill_detail_json(const SkillDetail& detail) {
    json assets = json::array();
    for (const auto& asset : detail.assets)
        assets.push_back({{"path", asset.path}, {"bytes", asset.bytes}, {"sha", asset.sha}});
    return json{{"id", detail.ref.id},
                {"name", detail.ref.name},
                {"source", detail.ref.source},
                {"installs", detail.ref.installs},
                {"description", detail.description},
                {"license", detail.license},
                {"category", detail.category},
                {"hash", detail.hash},
                {"tokens", detail.body.size() / 4},
                {"audited", false},
                {"files", assets}}
               .dump(2) +
           "\n";
}

std::string skill_list_text(const std::vector<InstalledPackage>& packages) {
    if (packages.empty())
        return "no installed skills\n";
    std::ostringstream out;
    for (const auto& package : packages)
        out << package.scope << " · " << package.name << " · "
            << (package.id.empty() ? "unmanaged" : package.id) << " · " << package.state << " · "
            << package.files << " files\n";
    return out.str();
}

std::string skill_list_json(const std::vector<InstalledPackage>& packages) {
    json rows = json::array();
    for (const auto& package : packages)
        rows.push_back({{"name", package.name},
                        {"scope", package.scope},
                        {"id", package.id},
                        {"source", package.source},
                        {"ref", package.ref},
                        {"state", package.state},
                        {"files", package.files},
                        {"bytes", package.bytes},
                        {"enabled", package.enabled}});
    return json{{"skills", rows}, {"count", rows.size()}}.dump(2) + "\n";
}

int run_skill_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const SkillCliCommand cmd = parse_skill_cli(args);
    if (cmd.verb == SkillVerb::None || !cmd.error.empty()) {
        err << "skills: " << (cmd.error.empty() ? "a verb is required" : cmd.error) << "\n"
            << kUsage;
        return 2;
    }
    switch (cmd.verb) {
    case SkillVerb::Search:
        return run_search(cmd, out, err);
    case SkillVerb::List:
        return run_list(cmd, out);
    case SkillVerb::Show:
        return run_show(cmd, out, err);
    case SkillVerb::Install:
        return looks_like_pack(cmd.target) ? install_pack(cmd, out, err)
                                           : install_registry(cmd, out, err);
    case SkillVerb::Update:
        return run_update(cmd, out, err);
    case SkillVerb::Remove:
        return run_remove(cmd, out, err);
    case SkillVerb::Sources:
        return run_sources(cmd, out);
    case SkillVerb::None:
        break;
    }
    return 2;
}

} // namespace agent
