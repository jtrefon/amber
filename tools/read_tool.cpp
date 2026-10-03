
#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace agent {

namespace {

// The confined file path, or nullopt with the error already written to `r`.
std::optional<std::string> confined_read_path(const json& a, ToolResult& r) {
    if (!a.contains("path") || !a["path"].is_string()) {
        r.ok = false;
        r.error = "missing 'path'";
        return std::nullopt;
    }
    std::string path;
    std::string err;
    if (!Workspace::confine(a["path"].get<std::string>(), path, err)) {
        r.ok = false;
        r.error = err;
        return std::nullopt;
    }
    return path;
}

// offset/limit, clamped at both ends. The upper limit is a line-based hard
// ceiling: a single call must never pull tens of thousands of lines into the
// conversation (binary/generated files).
std::pair<long, long> clamped_range(const json& a) {
    const long offset = std::max(1L, a.value("offset", 1L));
    const long limit = std::min(2000L, std::max(1L, a.value("limit", 200L)));
    return {offset, limit};
}

// A FIFO/socket would block the agent forever on open()/read — reject special
// files up front (confinement is lexical and cannot see the file type).
bool is_special_file(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && !S_ISREG(st.st_mode);
}

// Refuse binary files up front: NUL bytes in the first chunk mean
// compressed/compiled content whose random newlines would flood the
// conversation with garbage (a .git object read once produced 32k "lines").
bool looks_binary(std::istream& in) {
    std::array<char, 4096> sniff{};
    in.read(sniff.data(), static_cast<std::streamsize>(sniff.size()));
    const std::streamsize got = in.gcount();
    in.clear();
    in.seekg(0);
    for (std::streamsize i = 0; i < got; ++i)
        if (sniff[static_cast<size_t>(i)] == '\0')
            return true;
    return false;
}

// A single enormous line (minified JS, base64 blob) must not flood the
// conversation — truncate with a visible marker.
std::string truncate_line(std::string line, std::size_t max_bytes) {
    if (line.size() <= max_bytes)
        return line;
    const std::size_t cut = line.size() - max_bytes;
    line.resize(max_bytes);
    line += " ...(" + std::to_string(cut) + " bytes truncated)...";
    return line;
}

struct Page {
    std::string text; // the numbered lines
    long lineno = 0;  // lines consumed from the file
    long printed = 0; // lines written
    long total = 0;   // lines in the file
};

Page collect_page(std::istream& in, long offset, long limit) {
    constexpr std::size_t kMaxLineBytes = 4096;
    Page page;
    std::stringstream out;
    std::string line;
    while (std::getline(in, line)) {
        ++page.total;
        if (page.lineno + 1 < offset) {
            ++page.lineno;
            continue;
        }
        if (page.printed >= limit)
            break;
        out << (page.lineno + 1) << ":\t" << truncate_line(line, kMaxLineBytes) << "\n";
        ++page.lineno;
        ++page.printed;
    }
    page.text = out.str();
    return page;
}

// A refusal: not ok, with a workspace-relative reason.
void refuse(ToolResult& r, const std::string& reason) {
    r.ok = false;
    r.error = reason;
}

// Open the file and reject anything that is not a plain readable text file.
// False with `r` filled in when it must be refused.
bool open_text_file(const std::string& path, std::ifstream& in, ToolResult& r) {
    if (is_special_file(path)) {
        refuse(r, "not a regular file (named pipe/device) - refusing to block: " +
                      Workspace::relative(path));
        return false;
    }
    in.open(path, std::ios::binary);
    if (!in) {
        refuse(r, "cannot open: " + Workspace::relative(path));
        return false;
    }
    if (looks_binary(in)) {
        refuse(r, "binary file (NUL bytes) - refusing to read: " + Workspace::relative(path));
        return false;
    }
    return true;
}

// The page header, the numbered text, and the more/end trailer.
std::string render_page(const Page& page, const std::string& rel, long offset, bool more) {
    const long end_line = offset + page.printed - 1;
    std::string out = "# " + rel + ":" + std::to_string(offset) + "-" + std::to_string(end_line) +
                      " (" + std::to_string(page.printed) + " lines)\n" + page.text;
    if (more)
        out += "\n[more lines available: " + std::to_string(page.total - page.lineno) +
               " remaining; pass offset=" + std::to_string(page.lineno + 1) + " to continue]";
    else
        out += "\n[end of file: " + std::to_string(page.total) + " lines]";
    return out;
}

} // namespace

// read: paginated file reader. Args:
//   path     (string, required) file to read
//   offset   (int, optional) 1-based line number to start from (default 1)
//   limit    (int, optional) max lines to return (default 200)
// Returns the slice plus a note when more lines remain.
class ReadTool : public Tool {
public:
    std::string name() const noexcept override { return "read"; }

    bool is_read_only() const noexcept override { return true; }

    std::string description() const noexcept override {
        return "Read a text file with pagination. Returns lines [offset, "
               "offset+limit) and reports whether more lines follow so the "
               "model can page through large files.";
    }

    json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"path", {{"type", "string"}, {"description", "Path to the file to read"}}},
                  {"offset",
                   {{"type", "integer"}, {"description", "1-based starting line (default 1)"}}},
                  {"limit",
                   {{"type", "integer"}, {"description", "Max lines to return (default 200)"}}}}},
                {"required", {"path"}}};
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        const std::optional<std::string> path = confined_read_path(a, r);
        if (!path)
            return r;
        const auto [offset, limit] = clamped_range(a);

        std::ifstream in;
        if (!open_text_file(*path, in, r))
            return r;

        const Page page = collect_page(in, offset, limit);
        const std::string rel = Workspace::relative(*path);
        const bool more = page.printed >= limit && page.lineno < page.total;
        r.output = render_page(page, rel, offset, more);
        r.meta = {{"lines", page.printed},
                  {"total", page.total},
                  {"more", more},
                  {"path", rel},
                  {"start", offset}};
        return r;
    }
};

std::unique_ptr<Tool> make_read_tool() {
    return std::make_unique<ReadTool>();
}

} // namespace agent
