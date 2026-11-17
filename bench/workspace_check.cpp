#include "bench/workspace_check.h"

#include <fstream>
#include <regex>
#include <sstream>

#include "agent/text_diff.h"

namespace bench {

namespace {

bool read_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty())
            out += "\n";
        out += line;
    }
    return out;
}

std::string setup_text(const Scenario& scenario, const std::string& path) {
    if (!scenario.setup.is_object() || !scenario.setup.contains("files"))
        return {};
    const agent::json& files = scenario.setup["files"];
    if (!files.is_object() || !files.contains(path) || !files[path].is_string())
        return {};
    return files[path].get<std::string>();
}

void check_matcher(const std::string& path, const agent::json& matcher, const std::string& content,
                   std::vector<std::string>& failures) {
    if (matcher.is_string()) {
        if (content != matcher.get<std::string>())
            failures.push_back("workspace: " + path + " does not match exact content");
        return;
    }
    if (matcher.contains("exact") && content != matcher["exact"].get<std::string>())
        failures.push_back("workspace: " + path + " does not match exact content");
    if (matcher.contains("contains") &&
        content.find(matcher["contains"].get<std::string>()) == std::string::npos)
        failures.push_back("workspace: " + path + " does not contain the expected text");
    if (matcher.contains("regex")) {
        const std::regex pattern(matcher["regex"].get<std::string>());
        if (!std::regex_search(content, pattern))
            failures.push_back("workspace: " + path + " does not match the expected pattern");
    }
}

} // namespace

void check_files(const agent::json& expected, const std::filesystem::path& workspace,
                 std::vector<std::string>& failures) {
    if (!expected.contains("files") || !expected["files"].is_object())
        return;
    for (auto it = expected["files"].begin(); it != expected["files"].end(); ++it) {
        const std::filesystem::path path = workspace / it.key();
        std::string content;
        if (!read_file(path, content)) {
            failures.push_back("workspace: missing file " + it.key());
            continue;
        }
        check_matcher(it.key(), it.value(), content, failures);
    }
}

void check_absent(const agent::json& expected, const std::filesystem::path& workspace,
                  std::vector<std::string>& failures) {
    if (!expected.contains("absent") || !expected["absent"].is_array())
        return;
    for (const auto& item : expected["absent"]) {
        if (std::filesystem::exists(workspace / item.get<std::string>()))
            failures.push_back("workspace: unexpected file " + item.get<std::string>());
    }
}

void check_diffs(const Scenario& scenario, const agent::json& expected,
                 const std::filesystem::path& workspace, std::vector<std::string>& failures) {
    if (!expected.contains("diff_contains") || !expected["diff_contains"].is_object())
        return;
    for (auto it = expected["diff_contains"].begin(); it != expected["diff_contains"].end(); ++it) {
        std::string after;
        if (!read_file(workspace / it.key(), after)) {
            failures.push_back("workspace: missing file for diff " + it.key());
            continue;
        }
        const std::string diff =
            join(agent::render_unified_diff(it.key(), setup_text(scenario, it.key()), after));
        for (const auto& needle : it.value())
            if (diff.find(needle.get<std::string>()) == std::string::npos)
                failures.push_back("workspace: diff for " + it.key() +
                                   " does not contain the expected text");
    }
}

std::vector<std::string> check_expected_workspace(const Scenario& scenario,
                                                  const std::filesystem::path& workspace) {
    std::vector<std::string> failures;
    const agent::json& expected = scenario.expected_workspace;
    if (!expected.is_object() || expected.empty())
        return failures;
    check_files(expected, workspace, failures);
    check_absent(expected, workspace, failures);
    check_diffs(scenario, expected, workspace, failures);
    return failures;
}

} // namespace bench
