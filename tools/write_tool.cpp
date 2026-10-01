
#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace agent {

namespace {

// The confined target path, or nullopt with the error already written to `r`.
std::optional<std::string> confined_write_path(const json& a, ToolResult& r) {
    if (!a.contains("path") || !a["path"].is_string()) {
        r.ok = false;
        r.error = "missing 'path'";
        return std::nullopt;
    }
    if (!a.contains("edits") || !a["edits"].is_array() || a["edits"].empty()) {
        r.ok = false;
        r.error = "missing non-empty 'edits'";
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

std::string read_existing(const std::string& path) {
    std::ifstream fin(path);
    if (!fin)
        return {};
    return std::string((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
}

// Apply the edits in order. An empty 'old' is a full overwrite; a missing 'old'
// aborts, reporting which edit failed. Returns how many were applied.
std::optional<size_t> apply_edits(std::string& content, const json& edits, ToolResult& r) {
    size_t applied = 0;
    for (const auto& e : edits) {
        const std::string old_s = e.value("old", "");
        const std::string new_s = e.value("new", "");
        if (old_s.empty()) {
            content = new_s; // full overwrite / create
            ++applied;
            continue;
        }
        const size_t pos = content.find(old_s);
        if (pos == std::string::npos) {
            r.ok = false;
            r.error = "edit " + std::to_string(applied) + " not applied: 'old' not found";
            return std::nullopt;
        }
        content.replace(pos, old_s.size(), new_s);
        ++applied;
    }
    return applied;
}

} // namespace

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
            {"properties",
             {{"path", {{"type", "string"}, {"description", "File to edit or create"}}},
              {"edits",
               {{"type", "array"},
                {"description", "List of {old, new} edit objects"},
                {"items",
                 {{"type", "object"},
                  {"properties", {{"old", {{"type", "string"}}}, {"new", {{"type", "string"}}}}},
                  {"required", {"old", "new"}}}}}}}},
            {"required", {"path", "edits"}}};
    }

    // Create/overwrite (any edit with old=="") is a state change the user
    // should see; in-place patches of existing files are the common agent
    // workflow and run free in WRITE mode.
    bool requires_approval(const json& a) const noexcept override {
        try {
            if (!a.contains("edits") || !a["edits"].is_array())
                return true;
            return std::any_of(a["edits"].begin(), a["edits"].end(), [](const json& e) -> bool {
                auto it = e.find("old");
                if (it == e.end())
                    return true;
                const auto* s = it->get_ptr<const std::string*>();
                return s != nullptr && s->empty();
            });
        } catch (...) {
            return true; // fail-safe: require approval on any error
        }
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        const std::optional<std::string> path = confined_write_path(a, r);
        if (!path)
            return r;

        std::string content = read_existing(*path);
        const std::optional<size_t> applied = apply_edits(content, a["edits"], r);
        if (!applied)
            return r;

        std::ofstream fout(*path, std::ios::trunc);
        if (!fout) {
            r.ok = false;
            r.error = "cannot write: " + *path;
            return r;
        }
        fout << content;
        const std::string rel = Workspace::relative(*path);
        r.meta = {{"applied", *applied}, {"path", rel}};
        r.output = "applied " + std::to_string(*applied) + " edit(s) to " + rel;
        return r;
    }
};

std::unique_ptr<Tool> make_write_tool() {
    return std::make_unique<WriteTool>();
}

} // namespace agent
