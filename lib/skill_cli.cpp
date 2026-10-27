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
#include <memory>
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
    if (!is_kebab_name(name))
        return {};
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    if (ec)
        return {};
    const std::filesystem::path file =
        std::filesystem::weakly_canonical(base / name / "SKILL.md", ec);
    if (ec || file.string().rfind(base.string() + "/", 0) != 0)
        return {};
    std::ifstream f(file, std::ios::binary);
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
        package.state = "enabled";
        for (const auto& catalog_entry : entries) {
            if (catalog_entry.name == name && !catalog_entry.state.empty()) {
                package.state = catalog_entry.state;
                break;
            }
        }
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

Config load_acquisition_config() {
    Config cfg;
    if (std::ifstream conf("amber.conf"); conf)
        cfg.load_workspace_defaults("amber.conf");
    // Session overrides for scripts and CI. The TUI's per-project settings file
    // is deliberately not read here: the headless surface takes its config from
    // amber.conf and these variables, both of which a script can control.
    if (const char* scope = std::getenv("AMBER_SKILLS_INSTALL_SCOPE");
        scope && (*scope == 'p' || *scope == 'g'))
        cfg.skills_install_scope = *scope == 'g' ? "global" : "project";
    if (const char* index = std::getenv("AMBER_SKILLS_INDEX"); index && *index)
        cfg.skills_index = index;
    cfg.apply_environment();
    return cfg;
}

std::string env_or(const std::string& name, const char* fallback) {
    if (const char* value = std::getenv(name.c_str()); value && *value)
        return value;
    if (fallback) {
        if (const char* value = std::getenv(fallback); value && *value)
            return value;
    }
    return {};
}

SkillIndexOptions index_options(const Config& cfg) {
    SkillIndexOptions options;
    options.cache_root = skills_index_cache_dir();
    if (cfg.skills_index == "none")
        options.base_url.clear();
    else if (cfg.skills_index != "skills.sh")
        options.base_url = cfg.skills_index;
    if (const char* base = std::getenv("AMBER_SKILLS_INDEX_URL"); base && *base)
        options.base_url = base;
    options.token = env_or(cfg.skills_index_token_env, "SKILLS_SH_TOKEN");
    return options;
}

GithubOptions github_options(const Config& cfg) {
    GithubOptions options;
    if (const char* api = std::getenv("AMBER_GITHUB_API_URL"); api && *api)
        options.api_base = api;
    if (const char* raw = std::getenv("AMBER_GITHUB_RAW_URL"); raw && *raw)
        options.raw_base = raw;
    options.token = env_or(cfg.skills_github_token_env, "SKILLS_SH_TOKEN");
    return options;
}

std::string install_scope(const SkillCliCommand& cmd, const Config& cfg) {
    if (cmd.global)
        return "global";
    if (cmd.project)
        return "project";
    return cfg.skills_install_scope == "global" ? "global" : "project";
}

int run_search(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err,
               const SkillCliBackends& backends) {
    const IndexResult result = backends.search(cmd.target, cmd.limit);
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

int run_show(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err,
             const SkillCliBackends& backends) {
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
    const DetailResult detail = backends.detail(ref);
    if (!detail.detail) {
        err << "skills show: " << detail.error.detail << "\n";
        return 1;
    }
    const SkillDetail& found = *detail.detail;
    out << (cmd.json ? skill_detail_json(found) : skill_detail_text(found));
    return 0;
}

int install_pack(const SkillCliCommand& cmd, const Config& cfg, std::ostream& out,
                 std::ostream& err) {
    const std::string root = scope_root(install_scope(cmd, cfg));
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

int write_install(const SkillCliCommand& cmd, const Config& cfg, const SkillDetail& detail,
                  std::ostream& out, std::ostream& err, const SkillCliBackends& backends) {
    const AssetsResult assets = backends.assets(detail);
    if (!assets.files) {
        err << "skills install: " << assets.error.detail << "\n";
        return 1;
    }
    SkillInstallRequest request;
    request.name = skill_slug(detail.ref.id);
    request.dest_root = scope_root(install_scope(cmd, cfg));
    for (const auto& file : *assets.files)
        request.files.push_back({file.path, file.contents});
    const SkillInstallResult result = backends.install(request);
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
                    {"id", detail.ref.id},
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

int install_registry(const SkillCliCommand& cmd, const Config& cfg, std::ostream& out,
                     std::ostream& err, const SkillCliBackends& backends) {
    SkillRef ref;
    std::string error;
    if (!split_target(cmd.target, ref, error)) {
        err << "skills install: " << error << "\n";
        return 2;
    }
    const DetailResult detail = backends.detail(ref);
    if (!detail.detail) {
        err << "skills install: " << detail.error.detail << "\n";
        return 1;
    }
    if (const int rc = confirm_unattended(cmd, ref, err); rc != 0)
        return rc;
    return write_install(cmd, cfg, *detail.detail, out, err, backends);
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

UpdatePlan plan_update(const SkillCliBackends& backends, const InstalledPackage& package) {
    UpdatePlan plan;
    const DetailResult detail = backends.detail(ref_of(package));
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

std::string apply_plan(const SkillCliBackends& backends, const InstalledPackage& package,
                       const SkillDetail& detail) {
    const AssetsResult assets = backends.assets(detail);
    if (!assets.files)
        return "error: " + assets.error.detail;
    SkillInstallRequest request;
    request.name = package.name;
    request.dest_root = scope_root(package.scope);
    for (const auto& file : *assets.files)
        request.files.push_back({file.path, file.contents});
    request.replace = true;
    const SkillInstallResult result = backends.install(request);
    if (!result.ok)
        return "error: " + result.error;
    SkillLedger ledger = SkillLedger::for_root(request.dest_root);
    ledger.load();
    ledger.upsert(package.name,
                  ledger_entry_for(detail, package.name, request.files, result, package.enabled));
    return "updated";
}

std::string update_status(const SkillCliCommand& cmd, const SkillCliBackends& backends,
                          const InstalledPackage& package, const UpdatePlan& plan, int& failures) {
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
    const std::string status = apply_plan(backends, package, plan.detail);
    if (status.rfind("error:", 0) == 0)
        ++failures;
    return status;
}

int run_update(const SkillCliCommand& cmd, std::ostream& out, std::ostream& err,
               const SkillCliBackends& backends) {
    (void)err; // failures are reported per row, not as a single line
    Config cfg;
    SkillCatalog catalog(cfg);
    const auto entries = catalog.entries();
    std::vector<InstalledPackage> packages = packages_from("project", entries);
    const auto global_packages = packages_from("global", entries);
    packages.insert(packages.end(), global_packages.begin(), global_packages.end());

    json report = json::array();
    int failures = 0;
    for (const auto& package : packages) {
        if (!cmd.all && !cmd.target.empty() && package.name != cmd.target)
            continue;
        if (cmd.global && package.scope != "global")
            continue;
        const UpdatePlan plan = plan_update(backends, package);
        const std::string status = update_status(cmd, backends, package, plan, failures);
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

int run_remove(const SkillCliCommand& cmd, const Config& cfg, std::ostream& out,
               std::ostream& err) {
    if (cmd.target.empty()) {
        err << "skills remove: a skill name is required\n";
        return 2;
    }
    const std::string scope = install_scope(cmd, cfg);
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
    const Config cfg = load_acquisition_config();
    if (refresh) {
        std::error_code ec;
        fs::remove_all(skills_index_cache_dir(), ec);
    }
    long long bytes = 0;
    SkillSources sources;
    sources.index_base = index_options(cfg).base_url;
    sources.github_api = github_options(cfg).api_base;
    sources.github_raw = github_options(cfg).raw_base;
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
    else if (arg == "--project")
        cmd.project = true;
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
    SkillCliCommand cmd{};
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

SkillCliBackends default_backends(const Config& cfg) {
    auto index = std::make_shared<SkillsShIndex>(index_options(cfg));
    auto github = std::make_shared<GitHubIndex>(github_options(cfg));
    SkillCliBackends backends;
    backends.search = [index](const std::string& query, int limit) {
        return index->search(query, limit, CancellationToken{});
    };
    backends.detail = [github](const SkillRef& ref) {
        return github->detail(ref, CancellationToken{});
    };
    backends.assets = [github](const SkillDetail& detail) {
        return github->fetch_assets(detail, CancellationToken{});
    };
    backends.install = [](const SkillInstallRequest& request) {
        return install_skill_files(request);
    };
    return backends;
}

int run_skill_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  const SkillCliBackends& backends) {
    const SkillCliCommand cmd = parse_skill_cli(args);
    if (cmd.verb == SkillVerb::None || !cmd.error.empty()) {
        err << "skills: " << (cmd.error.empty() ? "a verb is required" : cmd.error) << "\n"
            << kUsage;
        return 2;
    }
    const Config settings = load_acquisition_config();
    const SkillCliBackends wired = backends.search ? backends : default_backends(settings);
    switch (cmd.verb) {
    case SkillVerb::Search:
        return run_search(cmd, out, err, wired);
    case SkillVerb::List:
        return run_list(cmd, out);
    case SkillVerb::Show:
        return run_show(cmd, out, err, wired);
    case SkillVerb::Install:
        return looks_like_pack(cmd.target) ? install_pack(cmd, settings, out, err)
                                           : install_registry(cmd, settings, out, err, wired);
    case SkillVerb::Update:
        return run_update(cmd, out, err, wired);
    case SkillVerb::Remove:
        return run_remove(cmd, settings, out, err);
    case SkillVerb::Sources:
        return run_sources(cmd, out);
    case SkillVerb::None:
        break;
    }
    return 2;
}

} // namespace agent
