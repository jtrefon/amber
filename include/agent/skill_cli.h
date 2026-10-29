#ifndef AGENT_SKILL_CLI_H
#define AGENT_SKILL_CLI_H

#include "agent/skill_install.h"
#include "agent/skill_index.h"
#include "agent/skill_ledger.h"

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace agent {

// The headless half of the acquisition surface (issue #400). Parsing and
// rendering are pure so they are tested without a socket; `run_skill_cli`
// wires the adapters and the ledger.
enum class SkillVerb : unsigned char {
    None,
    Search,
    List,
    Show,
    Install,
    Update,
    Remove,
    Sources,
};

struct SkillCliCommand {
    SkillVerb verb = SkillVerb::None;
    std::string target; // query, id, or skill name
    int limit = 20;
    bool json{false};
    bool global = false;
    bool project = false; // overrides a global default for one install
    bool yes = false;
    bool all = false;
    bool check = false;
    bool force = false; // apply over local edits the user has reviewed
    bool diff = false;  // print the review instead of applying
    bool refresh = false;
    std::string error; // non-empty = usage error
};

// `args` are the words after `skills`.
SkillCliCommand parse_skill_cli(const std::vector<std::string>& args);

// One installed package as `list`/`update` see it: the ledger entry plus the
// scope label and the live curation state.
struct InstalledPackage {
    std::string name;
    std::string scope; // project | global
    std::string id;
    std::string source;
    std::string ref;
    std::string hash;
    std::string local_hash;
    std::string state; // enabled | disabled | blocked
    int files = 0;
    long long bytes = 0;
    bool enabled = true;
};

// Where the index and the enrichments point, plus a one-line cache summary.
// `refresh` empties the regenerable cache first. Shared by the CLI's `sources`
// verb and the explorer's `/skills sources`.
struct SkillSources {
    std::string index_base;
    std::string github_api;
    std::string github_raw;
    std::string cache_dir;
    std::string cache_summary;
};

SkillSources skill_sources(bool refresh);

std::string skill_search_text(const SkillPage& page);
std::string skill_search_json(const SkillPage& page);
std::string skill_detail_text(const SkillDetail& detail);
std::string skill_detail_json(const SkillDetail& detail);
std::string skill_list_text(const std::vector<InstalledPackage>& packages);
std::string skill_list_json(const std::vector<InstalledPackage>& packages);

// The adapters the CLI needs, injectable so a test can drive every verb
// without a socket. Empty members default to the env-configured adapters.
struct SkillCliBackends {
    std::function<IndexResult(const std::string&, int)> search;
    std::function<DetailResult(const SkillRef&)> detail;
    std::function<AssetsResult(const SkillDetail&)> assets;
    // Every skill in a repository, for a bulk install. Empty plus `error` when
    // the repository has none.
    std::function<std::vector<SkillRef>(const std::string&, std::string&)> list_repo;
    std::function<SkillInstallResult(const SkillInstallRequest&)> install;
};

// Returns the process exit code: 0 ok, 1 execution failure, 2 usage error.
int run_skill_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  const SkillCliBackends& backends = {});

} // namespace agent

#endif // AGENT_SKILL_CLI_H
