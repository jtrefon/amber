// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace agent {

// write: patch-style editor. Applies a list of edits to a file.
// Args:
//   path  (string, required) target file
//   edits (array, required)  list of {old, new} blocks; each is replaced once
//                            in order. Use old="" with new=whole content to
//                            create/overwrite a file.
// This avoids sending full-file contents back and forth.
class WriteTool : public Tool {
public:
    std::string name() const noexcept override { return "write"; }

    std::string description() const noexcept override {
        return "Apply a patch-style edit to a file. Provide a list of edits, "
               "each with an 'old' substring to find and a 'new' replacement. "
               "To create or fully overwrite a file, use old=\"\" and put the "
               "entire contents in 'new'. Edits are applied sequentially.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"},
                          {"description", "File to edit or create"}}},
                {"edits", {{"type", "array"},
                           {"description", "List of {old, new} edit objects"},
                           {"items", {
                               {"type", "object"},
                               {"properties", {
                                   {"old", {{"type", "string"}}},
                                   {"new", {{"type", "string"}}}
                               }},
                               {"required", {"old", "new"}}
                           }}}}
            }},
            {"required", {"path", "edits"}}
        };
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("path") || !a["path"].is_string()) {
            r.ok = false; r.error = "missing 'path'"; return r;
        }
        if (!a.contains("edits") || !a["edits"].is_array() || a["edits"].empty()) {
            r.ok = false; r.error = "missing non-empty 'edits'"; return r;
        }
        std::string requested = a["path"].get<std::string>();
        std::string path, cerr;
        if (!Workspace::confine(requested, path, cerr)) {
            r.ok = false; r.error = cerr; return r;
        }

        std::ifstream fin(path);
        std::string content = fin ? std::string((std::istreambuf_iterator<char>(fin)),
                                                 std::istreambuf_iterator<char>()) : "";

        size_t applied = 0;
        for (const auto& e : a["edits"]) {
            std::string old_s = e.value("old", "");
            std::string new_s = e.value("new", "");
            if (old_s.empty()) {
                content = new_s;            // full overwrite / create
                ++applied;
                continue;
            }
            size_t pos = content.find(old_s);
            if (pos == std::string::npos) {
                r.ok = false;
                r.error = "edit " + std::to_string(applied) +
                          " not applied: 'old' not found";
                return r;
            }
            content.replace(pos, old_s.size(), new_s);
            ++applied;
        }

        std::ofstream fout(path, std::ios::trunc);
        if (!fout) { r.ok = false; r.error = "cannot write: " + path; return r; }
        fout << content;
        r.meta = {{"applied", applied}, {"path", path}};
        r.output = "applied " + std::to_string(applied) + " edit(s) to " + path;
        return r;
     }
};

std::unique_ptr<Tool> make_write_tool() {
    return std::make_unique<WriteTool>();
}

} // namespace agent
