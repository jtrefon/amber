#include "index_store.h"

#include "agent/workspace.h"
#include "project_index_plugin.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>

namespace agent::plugins::index_store {

namespace {

namespace fs = std::filesystem;

// The refusal states the rule rather than only the verdict: the index is
// carried in every prompt and read by anyone with the state dir, so a value
// never goes in -- a pointer does, and the environment expands it.
constexpr const char* kSecretRefusal =
    "refused: that looks like a credential. The index is carried in every prompt and read by "
    "anyone with the state dir, so record a pointer instead — a variable the environment expands, "
    "the file, the keychain entry — and let the tool that needs it read the value.";

std::string stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string first_line(const std::string& text) {
    const std::size_t at = text.find('\n');
    return at == std::string::npos ? text : text.substr(0, at);
}

bool blank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string default_title() {
    const std::string name = fs::path(Workspace::root()).filename().string();
    return "# Project: " + (name.empty() ? std::string("this project") : name) + "\n";
}

void append_log(const std::string& detail) {
    const std::string path = log_path();
    if (path.empty())
        return;
    std::ofstream out(path, std::ios::app);
    if (!out)
        return;
    out << stamp() << " " << detail << "\n";
}

// Every writer commits through here, so the redaction rule and the log cannot
// be forgotten by one of them.
bool commit(const std::string& body, const std::string& detail, std::string& error) {
    if (looks_like_secret(body)) {
        error = kSecretRefusal;
        return false;
    }
    const std::string file = ProjectIndexPlugin::file_path();
    if (file.empty()) {
        error = "this project has no state directory to write the index into";
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + file;
        return false;
    }
    out << body;
    append_log(detail);
    return true;
}

// A line-based rewrite: one entry is one line, so a hand edit is as valid as a
// tool edit and neither has to parse the other.
bool rewrite(bool remove, const std::string& find, const std::string& replacement,
             std::string& error) {
    if (blank(find)) {
        error = "give me the text of the line to match";
        return false;
    }
    const std::string body = read_all();
    std::istringstream lines(body);
    std::string line;
    std::string out;
    bool matched = false;
    while (std::getline(lines, line)) {
        if (!matched && line.find(find) != std::string::npos) {
            matched = true;
            if (!remove)
                out += replacement + "\n";
            continue;
        }
        out += line + "\n";
    }
    if (!matched) {
        error = "no line contains '" + find + "'";
        return false;
    }
    return commit(out, (remove ? "remove: " : "update: ") + first_line(replacement), error);
}

} // namespace

std::string read_all() {
    const std::string file = ProjectIndexPlugin::file_path();
    if (file.empty())
        return {};
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_all(const std::string& body, std::string& error) {
    if (blank(body)) {
        error = "an empty index is not worth writing; drop the file instead";
        return false;
    }
    return commit(body, "write: " + first_line(body), error);
}

bool add_entry(const std::string& section, const std::string& entry, std::string& error) {
    if (blank(entry)) {
        error = "an entry needs text";
        return false;
    }
    std::string body = read_all();
    if (blank(body))
        body = default_title();
    const std::string heading = "## " + section_title(section);
    const std::size_t at = body.find(heading);
    if (at == std::string::npos) {
        if (body.empty() || body.back() != '\n')
            body += '\n';
        body += "\n" + heading + "\n" + entry + "\n";
    } else {
        // Inside the section, before whatever heading follows it: the entry
        // stays under the heading it was filed against.
        const std::size_t next = body.find("\n## ", at + heading.size());
        const std::size_t insert_at = next == std::string::npos ? body.size() : next + 1;
        body.insert(insert_at, entry + "\n");
    }
    return commit(body, "add " + section + ": " + first_line(entry), error);
}

bool update_line(const std::string& find, const std::string& entry, std::string& error) {
    if (blank(entry)) {
        error = "an entry needs text";
        return false;
    }
    return rewrite(/*remove=*/false, find, entry, error);
}

bool remove_line(const std::string& match, std::string& error) {
    return rewrite(/*remove=*/true, match, {}, error);
}

std::string section_title(const std::string& key) {
    if (key == "shape")
        return "Shape";
    if (key == "runs")
        return "How it runs";
    if (key == "where")
        return "Where things are";
    if (key == "selection")
        return "Library selection";
    return key;
}

bool looks_like_secret(const std::string& text) {
    static const std::regex kPatterns[] = {
        std::regex(R"(AKIA[0-9A-Z]{16})"),
        std::regex(R"(gh[pousr]_[A-Za-z0-9]{20,})"),
        std::regex(R"(github_pat_[A-Za-z0-9_]{20,})"),
        std::regex(R"(sk-[A-Za-z0-9_-]{20,})"),
        std::regex(R"(xox[abprs]-[A-Za-z0-9-]{10,})"),
        std::regex(R"(eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.)"),
        std::regex(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----)"),
        std::regex(
            R"([A-Za-z0-9_]*(TOKEN|SECRET|PASSWORD|PASSWD|API_?KEY)[A-Za-z0-9_]*[ \t]*[:=][ \t]*["']?[A-Za-z0-9/+_.-]{16,})"),
    };
    return std::any_of(
        std::begin(kPatterns), std::end(kPatterns),
        [&text](const std::regex& pattern) { return std::regex_search(text, pattern); });
}

std::string log_path() {
    const std::string file = ProjectIndexPlugin::file_path();
    if (file.empty())
        return {};
    return (fs::path(file).parent_path() / "project-index.log").string();
}

} // namespace agent::plugins::index_store
