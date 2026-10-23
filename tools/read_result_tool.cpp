#include "agent/artifacts.h"
#include "agent/tool.h"
#include "agent/tools.h"

#include <algorithm>
#include <utility>

namespace agent {

namespace {

// Byte offset from a JSON integer; negatives clamp to zero.
std::size_t read_offset(const json& args) {
    if (!args.contains("offset") || !args["offset"].is_number_integer())
        return 0;
    const long long raw = args["offset"].get<long long>();
    return raw > 0 ? static_cast<std::size_t>(raw) : 0;
}

// Byte limit, clamped to [1, kArtifactReadMax]; absent means the default.
std::size_t read_limit(const json& args) {
    if (!args.contains("limit") || !args["limit"].is_number_integer())
        return kArtifactReadDefault;
    const long long raw = args["limit"].get<long long>();
    if (raw <= 0)
        return kArtifactReadDefault;
    return std::min(static_cast<std::size_t>(raw), kArtifactReadMax);
}

std::string read_grep(const json& args) {
    if (!args.contains("grep") || !args["grep"].is_string())
        return {};
    return args["grep"].get<std::string>();
}

class ReadResultTool final : public Tool {
public:
    std::string name() const noexcept override { return "read_result"; }

    std::string description() const noexcept override {
        return "Read part of a tool output that was too large for the context "
               "budget, or search it by substring.";
    }

    json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"result_id",
                   {{"type", "string"},
                    {"description", "Handle reported by a truncated tool result."}}},
                  {"offset", {{"type", "integer"}, {"description", "Byte offset (default 0)."}}},
                  {"limit",
                   {{"type", "integer"},
                    {"description", "Maximum bytes to return (default 16384, max 65536)."}}},
                  {"grep",
                   {{"type", "string"},
                    {"description",
                     "Return matching lines with line numbers instead of a byte range."}}}}},
                {"required", {"result_id"}}};
    }

    bool is_read_only() const noexcept override { return true; }

    ToolResult execute(const json& args) const override {
        ToolResult result;
        if (!args.contains("result_id") || !args["result_id"].is_string()) {
            result.ok = false;
            result.error = "missing 'result_id'";
            return result;
        }
        const std::string id = args["result_id"].get<std::string>();
        const std::size_t offset = read_offset(args);
        std::string error;
        auto output = read_tool_artifact(id, offset, read_limit(args), read_grep(args), error);
        if (!output) {
            result.ok = false;
            result.error = error;
            return result;
        }
        result.output = std::move(*output);
        result.meta = {{"result_id", id}, {"offset", offset}, {"bytes", result.output.size()}};
        return result;
    }
};

} // namespace

std::unique_ptr<Tool> make_read_result_tool() {
    return std::make_unique<ReadResultTool>();
}

} // namespace agent
