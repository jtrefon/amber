#include "index_tools.h"

#include "agent/workspace.h"
#include "index_store.h"
#include "project_index_plugin.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace agent::plugins {

namespace {

namespace fs = std::filesystem;

std::string string_arg(const json& args, const char* key) {
    if (!args.is_object() || !args.contains(key))
        return {};
    const json& value = args[key];
    return value.is_string() ? value.get<std::string>() : std::string();
}

std::string read_text(const std::string& rel) {
    std::ifstream in(fs::path(Workspace::root()) / rel, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool exists_at(const std::string& rel) {
    std::error_code ec;
    return fs::exists(fs::path(Workspace::root()) / rel, ec);
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0)
            out += ", ";
        out += items[i];
    }
    return out;
}

std::string lines_of(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items)
        out += item + "\n";
    return out;
}

// What the scan found, in the shape the index uses. Deliberately shallow: a
// presence check plus what the JSON manifests say. Compose service detail,
// shared containers, where a key lives -- the agent records those through
// index_add, because a guess written by a scanner reads exactly like a fact and
// is wrong in the same way.
struct Findings {
    std::vector<std::string> shape;
    std::vector<std::string> runs;
    std::vector<std::string> where;
    std::vector<std::string> tags;
    std::vector<std::string> sources;
};

void add_tag(Findings& f, const std::string& tag) {
    if (std::find(f.tags.begin(), f.tags.end(), tag) == f.tags.end())
        f.tags.emplace_back(tag);
}

void note_source(Findings& f, const std::string& rel) {
    if (std::find(f.sources.begin(), f.sources.end(), rel) == f.sources.end())
        f.sources.emplace_back(rel);
}

void scan_node(Findings& f) {
    const std::string text = read_text("package.json");
    if (text.empty())
        return;
    note_source(f, "package.json");
    add_tag(f, "#node");
    const json doc = json::parse(text, nullptr, false);
    if (!doc.is_object())
        return;
    std::vector<std::string> deps;
    for (const char* bag : {"dependencies", "devDependencies", "peerDependencies"}) {
        if (!doc.contains(bag) || !doc[bag].is_object())
            continue;
        for (auto it = doc[bag].begin(); it != doc[bag].end(); ++it)
            deps.emplace_back(it.key());
    }
    static const std::pair<const char*, const char*> kKnown[] = {
        {"next", "#next"},
        {"react", "#react"},
        {"@angular/core", "#angular"},
        {"vue", "#vue"},
        {"svelte", "#svelte"},
        {"tailwindcss", "#tailwind"},
        {"bootstrap", "#bootstrap"},
        {"vitest", "#vitest"},
        {"jest", "#jest"},
    };
    std::vector<std::string> named;
    for (const auto& dep : deps)
        for (const auto& known : kKnown)
            if (dep == known.first) {
                add_tag(f, known.second);
                named.emplace_back(known.first);
            }
    if (!named.empty())
        f.shape.emplace_back("node project (" + join(named) + ")");
}

void scan_php(Findings& f) {
    const std::string text = read_text("composer.json");
    if (text.empty())
        return;
    note_source(f, "composer.json");
    add_tag(f, "#php");
    add_tag(f, "#composer");
    static const std::pair<const char*, const char*> kKnown[] = {
        {"laravel/framework", "#laravel"},
        {"symfony/framework-bundle", "#symfony"},
        {"phpunit/phpunit", "#phpunit"},
        {"woocommerce/woocommerce", "#woocommerce"},
        {"johnpbloch/wordpress-core", "#wordpress"},
    };
    for (const auto& known : kKnown)
        if (text.find(known.first) != std::string::npos) {
            add_tag(f, known.second);
            f.shape.emplace_back(std::string("PHP: ") + known.first);
        }
}

void scan_python(Findings& f, const std::string& rel) {
    const std::string text = read_text(rel);
    if (text.empty())
        return;
    note_source(f, rel);
    add_tag(f, "#python");
    static const std::pair<const char*, const char*> kKnown[] = {
        {"django", "#django"}, {"fastapi", "#fastapi"}, {"flask", "#flask"}, {"pytest", "#pytest"}};
    for (const auto& known : kKnown)
        if (text.find(known.first) != std::string::npos) {
            add_tag(f, known.second);
            f.shape.emplace_back(std::string("python: ") + known.first);
        }
}

void scan_named(Findings& f, const std::string& rel, const char* tag, const char* label) {
    const std::string text = read_text(rel);
    if (text.empty())
        return;
    note_source(f, rel);
    add_tag(f, tag);
    f.shape.emplace_back(label);
}

void scan_csproj(Findings& f, const std::string& rel) {
    const std::string text = read_text(rel);
    if (text.empty())
        return;
    note_source(f, rel);
    add_tag(f, "#dotnet");
    if (text.find("EntityFrameworkCore") != std::string::npos)
        add_tag(f, "#efcore");
    const std::size_t at = text.find("<TargetFramework>");
    if (at == std::string::npos) {
        f.shape.emplace_back("solution: " + fs::path(rel).filename().string());
        return;
    }
    const std::size_t end = text.find('<', at + 17);
    const std::string tfm = text.substr(at + 17, end - (at + 17));
    f.shape.emplace_back("solution: " + fs::path(rel).filename().string() + " (" + tfm + ")");
}

// Up to three project files, at the root and in the directories a .NET solution
// usually keeps its projects: bounded, so a checkout cannot make the scan pay.
std::vector<std::string> csproj_files() {
    std::vector<std::string> found;
    for (const char* dir : {"", "src/", "services/", "apps/"}) {
        std::error_code ec;
        fs::directory_iterator it(fs::path(Workspace::root()) / dir, ec);
        if (ec)
            continue;
        for (const auto& entry : it) {
            const std::string name = entry.path().filename().string();
            if (name.size() > 7 && name.compare(name.size() - 7, 7, ".csproj") == 0)
                found.emplace_back(std::string(dir) + name);
            if (found.size() >= 3)
                return found;
        }
    }
    return found;
}

void scan_docker(Findings& f) {
    const bool compose = exists_at("docker-compose.yml") || exists_at("docker-compose.yaml") ||
                         exists_at("compose.yml");
    if (compose) {
        add_tag(f, "#dockercompose");
        f.shape.emplace_back("runs in docker compose");
        f.runs.emplace_back("up: docker compose up -d");
        f.where.emplace_back(
            "compose: docker-compose.yml — services and shared containers are worth "
            "recording");
    }
    if (exists_at("Dockerfile"))
        add_tag(f, "#docker");
}

void scan_build_files(Findings& f) {
    if (exists_at("Makefile"))
        f.runs.emplace_back("build: make");
    if (exists_at("CMakeLists.txt")) {
        add_tag(f, "#cmake");
        f.shape.emplace_back("C++ (CMake)");
    }
    if (exists_at("Cargo.toml"))
        scan_named(f, "Cargo.toml", "#rust", "Rust (cargo)");
    if (exists_at("go.mod"))
        scan_named(f, "go.mod", "#go", "Go modules");
}

void scan_web(Findings& f) {
    if (exists_at("package.json"))
        scan_node(f);
    if (exists_at("tsconfig.json")) {
        add_tag(f, "#typescript");
        f.where.emplace_back("typescript: tsconfig.json");
    }
}

void scan_other_manifests(Findings& f) {
    if (exists_at("composer.json"))
        scan_php(f);
    if (exists_at("requirements.txt"))
        scan_python(f, "requirements.txt");
    else if (exists_at("pyproject.toml"))
        scan_python(f, "pyproject.toml");
    for (const auto& rel : csproj_files())
        scan_csproj(f, rel);
    if (exists_at(".github/workflows"))
        f.runs.emplace_back("ci: GitHub Actions (.github/workflows)");
    if (exists_at(".env.example"))
        f.where.emplace_back("config: .env.example lists the variables the project needs");
}

// Four independent questions -- is it containerised, how is it built, what is
// the front end, what else is in the tree -- asked one at a time.
Findings scan_project() {
    Findings f;
    scan_docker(f);
    scan_build_files(f);
    scan_web(f);
    scan_other_manifests(f);
    return f;
}

std::string today() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[16] = {0};
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

std::string provenance(const std::vector<std::string>& sources) {
    std::vector<std::string> recorded;
    recorded.reserve(sources.size());
    for (const auto& rel : sources) {
        std::string resolved;
        std::string error;
        recorded.emplace_back(rel + "@" +
                              (Workspace::confine(rel, resolved, error)
                                   ? ProjectIndexPlugin::hash_file(resolved)
                                   : std::string()));
    }
    return join(recorded);
}

// The proposal, in the index's own shape, so the body that comes back through
// `commit` is written as it will be read.
std::string render_proposal(const Findings& f) {
    const std::string name = fs::path(Workspace::root()).filename().string();
    std::string out = "# Project: " + (name.empty() ? std::string("this project") : name) + "\n";
    out += "index v1 · refreshed " + today();
    if (!f.sources.empty())
        out += " · sources: " + provenance(f.sources);
    out += "\n";
    out += "\n## " + index_store::section_title("shape") + "\n";
    out += f.shape.empty() ? "(nothing detected — say what this project is)\n" : lines_of(f.shape);
    out += "\n## " + index_store::section_title("runs") + "\n";
    out += f.runs.empty() ? "(not detected — record how it starts, how tests run, and where its "
                            "logs go)\n"
                          : lines_of(f.runs);
    out += "\n## " + index_store::section_title("where") + "\n";
    out += lines_of(f.where);
    out += "artefacts: record keys, tokens and hosts by mechanism, never by value — for example "
           "$ADO_PAT in the dev container env\n";
    out += "\n## " + index_store::section_title("selection") + "\n";
    out += f.tags.empty() ? "(none detected)\n" : join(f.tags) + "\n";
    return out;
}

class IndexInitTool : public Tool {
public:
    std::string name() const noexcept override { return "index_init"; }
    std::string description() const noexcept override {
        return "Scan this project and propose the project index: how it runs, where its parts and "
               "artefacts live, which library entries apply. Called without `commit` it only "
               "proposes — confirm and correct the text with the user, then call it again with the "
               "final body in `commit` to write it. Worth offering when a compose file, a monorepo "
               "or several stacks make the value clear, or when the user asks.";
    }
    json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"commit",
                   {{"type", "string"},
                    {"description",
                     "The confirmed index body to write. Omit to get the proposal."}}}}}};
    }
    bool requires_approval(const json& args) const noexcept override {
        return args.is_object() && args.contains("commit");
    }
    std::string summarize(const json& args) const override {
        return requires_approval(args) ? "write the project index"
                                       : "scan the project for the index";
    }
    ToolResult execute(const json& args) const override;
};

ToolResult IndexInitTool::execute(const json& args) const {
    const std::string body = string_arg(args, "commit");
    if (body.empty())
        return ToolResult{
            true,
            render_proposal(scan_project()) +
                "\nConfirm this with the user, correct anything wrong, then call index_init again "
                "with the final body in `commit`.\n",
            {},
            nullptr};
    std::string error;
    if (!index_store::write_all(body, error))
        return ToolResult{false, {}, error, nullptr};
    return ToolResult{true,
                      "wrote " + ProjectIndexPlugin::file_path() +
                          "\nthe running session's block is unchanged; /index refresh re-reads it, "
                          "and the next session reads it anyway\n",
                      {},
                      nullptr};
}

class IndexAddTool : public Tool {
public:
    std::string name() const noexcept override { return "index_add"; }
    std::string description() const noexcept override {
        return "Record one fact in the project index: part of the shape, how something is run, "
               "where an artefact or configuration lives, or a library tag that should apply. "
               "Sections: shape, runs, where, selection — or a heading you name.";
    }
    json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"section",
                   {{"type", "string"},
                    {"description", "shape | runs | where | selection (default: shape)"}}},
                  {"entry", {{"type", "string"}, {"description", "The line to record"}}}}},
                {"required", {"entry"}}};
    }
    std::string summarize(const json& args) const override {
        return "record in the project index: " + string_arg(args, "entry");
    }
    ToolResult execute(const json& args) const override;
};

ToolResult IndexAddTool::execute(const json& args) const {
    const std::string entry = string_arg(args, "entry");
    std::string section = string_arg(args, "section");
    if (section.empty())
        section = "shape";
    std::string error;
    if (!index_store::add_entry(section, entry, error))
        return ToolResult{false, {}, error, nullptr};
    return ToolResult{true,
                      "recorded under ## " + index_store::section_title(section) + "\n" + entry +
                          "\nthe running session's block is unchanged; /index refresh re-reads it, "
                          "and the next session reads it anyway\n",
                      {},
                      nullptr};
}

class IndexUpdateTool : public Tool {
public:
    std::string name() const noexcept override { return "index_update"; }
    std::string description() const noexcept override {
        return "Replace the first line of the project index that contains `find` with `entry`. "
               "Use it when a fact changed — a framework moved, a container was replaced.";
    }
    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"find", {{"type", "string"}, {"description", "Text of the line to replace"}}},
              {"entry", {{"type", "string"}, {"description", "The line to put in its place"}}}}},
            {"required", {"find", "entry"}}};
    }
    std::string summarize(const json& args) const override {
        return "update the project index: " + string_arg(args, "find");
    }
    ToolResult execute(const json& args) const override;
};

ToolResult IndexUpdateTool::execute(const json& args) const {
    std::string error;
    if (!index_store::update_line(string_arg(args, "find"), string_arg(args, "entry"), error))
        return ToolResult{false, {}, error, nullptr};
    return ToolResult{true,
                      "updated\n" + string_arg(args, "entry") +
                          "\nthe running session's block is unchanged; /index refresh re-reads it, "
                          "and the next session reads it anyway\n",
                      {},
                      nullptr};
}

class IndexRemoveTool : public Tool {
public:
    std::string name() const noexcept override { return "index_remove"; }
    std::string description() const noexcept override {
        return "Remove the first line of the project index that contains `match` — a service that "
               "went away, a tag that no longer applies.";
    }
    json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"match", {{"type", "string"}, {"description", "Text of the line to remove"}}}}},
                {"required", {"match"}}};
    }
    std::string summarize(const json& args) const override {
        return "remove from the project index: " + string_arg(args, "match");
    }
    ToolResult execute(const json& args) const override;
};

ToolResult IndexRemoveTool::execute(const json& args) const {
    std::string error;
    const std::string match = string_arg(args, "match");
    if (!index_store::remove_line(match, error))
        return ToolResult{false, {}, error, nullptr};
    return ToolResult{true,
                      "removed: " + match +
                          "\nthe running session's block is unchanged; /index refresh re-reads it, "
                          "and the next session reads it anyway\n",
                      {},
                      nullptr};
}

class IndexShowTool : public Tool {
public:
    std::string name() const noexcept override { return "index_show"; }
    std::string description() const noexcept override {
        return "Print the project index as it stands on disk. The block in the prompt is frozen "
               "for the session, so this is how a change made during the session is read back.";
    }
    json parameters_schema() const override {
        return {{"type", "object"}, {"properties", json::object()}};
    }
    bool is_read_only() const noexcept override { return true; }
    ToolResult execute(const json&) const override;
};

ToolResult IndexShowTool::execute(const json&) const {
    const std::string body = index_store::read_all();
    if (body.empty())
        return ToolResult{
            true, "no project index yet — amber can write one (index_init)\n", {}, nullptr};
    return ToolResult{true, body, {}, nullptr};
}

} // namespace

std::vector<std::unique_ptr<Tool>> make_index_tools() {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.emplace_back(std::make_unique<IndexInitTool>());
    tools.emplace_back(std::make_unique<IndexAddTool>());
    tools.emplace_back(std::make_unique<IndexUpdateTool>());
    tools.emplace_back(std::make_unique<IndexRemoveTool>());
    tools.emplace_back(std::make_unique<IndexShowTool>());
    return tools;
}

} // namespace agent::plugins
