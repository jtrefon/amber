#include "agent/tools.h"

#include "agent/context_shape.h"
#include "agent/json_read.h"

namespace agent {

namespace {

ShapeOp parse_op(const std::string& op, bool& ok) {
    ok = true;
    if (op == "drop")
        return ShapeOp::Drop;
    if (op == "superseded")
        return ShapeOp::Superseded;
    ok = false;
    return ShapeOp::Drop;
}

std::vector<std::string> read_ids(const json& args) {
    std::vector<std::string> ids;
    if (!args.contains("result_ids") || !args["result_ids"].is_array())
        return ids;
    for (const auto& id : args["result_ids"])
        if (id.is_string() && !id.get<std::string>().empty())
            ids.push_back(id.get<std::string>());
    return ids;
}

} // namespace

class ContextTool : public Tool {
public:
    explicit ContextTool(ShapeQueue& shapes) : shapes_(shapes) {}

    std::string name() const noexcept override { return "context"; }

    std::string description() const noexcept override {
        return R"(Name tool results the conversation has stopped needing, so the harness can replace them with a digest you can read back. op="drop" takes result_ids (the result_id in a tool envelope); op="superseded" takes none and drops earlier reads of a path that was read again. Give a short reason. The harness applies the plan at the start of your next request, once per turn, and only when it frees enough to be worth the rebuild; read_result restores anything it replaced.)";
    }

    agent::json parameters_schema() const override {
        return {{"type", "object"},
                {"properties",
                 {{"op", {{"type", "string"}, {"enum", {"drop", "superseded"}}}},
                  {"result_ids",
                   {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 64}}},
                  {"reason", {{"type", "string"}}}}},
                {"required", {"op"}}};
    }

    bool is_read_only() const noexcept override { return false; }

    ToolResult execute(const agent::json& args) const override {
        const std::string op_name = read_string(args, "op", "");
        bool ok = false;
        const ShapeOp op = parse_op(op_name, ok);
        if (!ok)
            return ToolResult{false, "", R"(context: op must be "drop" or "superseded")",
                              agent::json::object()};
        ShapePlan plan;
        plan.op = op;
        plan.result_ids = read_ids(args);
        plan.reason = read_string(args, "reason", "");
        if (op == ShapeOp::Drop && plan.result_ids.empty())
            return ToolResult{false, "", "context: drop needs result_ids (one or more ids)",
                              agent::json::object()};
        shapes_.push(std::move(plan));
        const std::string what = op == ShapeOp::Superseded
                                     ? std::string("the superseded reads")
                                     : std::to_string(read_ids(args).size()) + " named result(s)";
        return ToolResult{true,
                          "queued: " + what +
                              " will be replaced at the start of your next request, if they are "
                              "still there and freeing them is worth the rebuild.",
                          "", agent::json::object()};
    }

private:
    ShapeQueue& shapes_;
};

std::unique_ptr<Tool> make_context_tool(ShapeQueue& shapes) {
    return std::make_unique<ContextTool>(shapes);
}

} // namespace agent
