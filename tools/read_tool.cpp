// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include <fstream>
#include <sstream>
#include <string>

namespace agent {

// read: paginated file reader. Args:
//   path     (string, required) file to read
//   offset   (int, optional) 1-based line number to start from (default 1)
//   limit    (int, optional) max lines to return (default 200)
// Returns the slice plus a note when more lines remain.
class ReadTool : public Tool {
public:
    std::string name() const override { return "read"; }

    std::string description() const override {
        return "Read a text file with pagination. Returns lines [offset, "
               "offset+limit) and reports whether more lines follow so the "
               "model can page through large files.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"},
                          {"description", "Path to the file to read"}}},
                {"offset", {{"type", "integer"},
                            {"description", "1-based starting line (default 1)"}}},
                {"limit", {{"type", "integer"},
                           {"description", "Max lines to return (default 200)"}}}
            }},
            {"required", {"path"}}
        };
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("path") || !a["path"].is_string()) {
            r.ok = false; r.error = "missing 'path'"; return r;
        }
        std::string requested = a["path"].get<std::string>();
        std::string path, cerr;
        if (!Workspace::confine(requested, path, cerr)) {
            r.ok = false; r.error = cerr; return r;
        }
        long offset = a.value("offset", 1L);
        long limit = a.value("limit", 200L);
        if (offset < 1) offset = 1;
        if (limit < 1) limit = 1;

        std::ifstream in(path);
        if (!in) { r.ok = false; r.error = "cannot open: " + path; return r; }

        std::string line;
        long lineno = 0;
        long printed = 0;
        long total = 0;
        std::stringstream out;
        while (std::getline(in, line)) {
            ++total;
            if (lineno + 1 < offset) { ++lineno; continue; }
            if (printed >= limit) break;
            out << (lineno + 1) << ":\t" << line << "\n";
            ++lineno; ++printed;
        }
        r.output = out.str();
        if (printed >= limit && lineno < total)
            r.output += "\n[more lines available: " + std::to_string(total - lineno)
                      + " remaining; pass offset=" + std::to_string(lineno + 1)
                      + " to continue]";
        else
            r.output += "\n[end of file: " + std::to_string(total) + " lines]";
        return r;
    }
};

std::unique_ptr<Tool> make_read_tool() {
    return std::make_unique<ReadTool>();
}

} // namespace agent
