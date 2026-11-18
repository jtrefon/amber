#include "agent/tools.h"

#include "agent/checkpoint_format.h"
#include "agent/checkpoint_service.h"
#include "agent/run_scope.h"
#include "agent/workspace.h"

#include <exception>
#include <string>

namespace agent {

namespace {

constexpr std::size_t kMaxDiff = std::size_t{64} * 1024;

std::string target_of(const json& args) {
    std::string target = args.value("target", std::string());
    if (target.empty())
        target = args.value("turn", std::string());
    if (target.empty())
        target = args.value("sha", std::string());
    return target;
}

} // namespace

class CheckpointTool final : public Tool {
public:
    std::string name() const noexcept override { return "git_checkpoint"; }

    std::string description() const noexcept override {
        return "Inspect and restore amber's per-turn working-tree checkpoints. `list` returns "
               "newest first with the turn, SHA, changed-file count and prompt; `diff` shows "
               "what a checkpoint changed; `restore` puts the tree back and records the revert, "
               "so it can be undone. Checkpoints live on refs/amber/checkpoints/<session>; the "
               "user's branch and index stay put. The session is the running window.";
    }

    json parameters_schema() const override {
        json p = json::object();
        p["type"] = "object";
        json props = json::object();
        props["action"] = json::object({{"type", "string"},
                                        {"enum", json::array({"list", "diff", "restore"})},
                                        {"description", "list | diff | restore"}});
        props["target"] = json::object(
            {{"type", "string"},
             {"description", "Turn number or SHA prefix. Required for restore; diff defaults to "
                             "the latest checkpoint."}});
        p["properties"] = props;
        p["required"] = json::array({"action"});
        return p;
    }

    bool requires_approval(const json& args) const noexcept override {
        return args.is_object() && args.contains("action") && args["action"].is_string() &&
               args["action"].get<std::string>() == "restore";
    }

    bool is_read_only() const noexcept override { return false; }

    std::string summarize(const json& args) const override {
        const std::string action = args.value("action", std::string("list"));
        const std::string target = target_of(args);
        if (action == "restore")
            return "restore the working tree to checkpoint " +
                   (target.empty() ? std::string("(missing target)") : target);
        return target.empty() ? "checkpoint " + action : "checkpoint " + action + " " + target;
    }

    ToolResult execute(const json& args) const override {
        try {
            return run(args);
        } catch (const std::exception& e) {
            return {false, "", std::string("git_checkpoint: ") + e.what()};
        } catch (...) {
            return {false, "", "git_checkpoint: unknown failure"};
        }
    }

private:
    ToolResult run(const json& args) const {
        const std::string action = args.value("action", std::string("list"));
        const std::string target = target_of(args);
        CheckpointService service(Workspace::workspace_root(), run_session_id());
        if (action == "list")
            return list(service);
        if (action == "diff")
            return diff(service, target);
        if (action == "restore")
            return restore(service, target);
        return {false, "", "git_checkpoint: action must be list, diff or restore"};
    }

    static ToolResult list(const CheckpointService& service) {
        std::string error;
        const auto checkpoints = service.history(error);
        if (!error.empty())
            return {false, "", "git_checkpoint list: " + error};
        if (checkpoints.empty())
            return {true, "(no checkpoints yet)", "", {{"count", 0}}};

        json items = json::array();
        for (const auto& checkpoint : checkpoints) {
            items.push_back({{"turn", checkpoint.turn},
                             {"sha", checkpoint.id},
                             {"pre", checkpoint.pre()},
                             {"files", checkpoint.files},
                             {"prompt", checkpoint.prompt},
                             {"timestamp_s", checkpoint.timestamp_s}});
        }
        return {true,
                checkpoint_history_text(checkpoints),
                "",
                {{"count", checkpoints.size()}, {"checkpoints", std::move(items)}}};
    }

    static ToolResult diff(const CheckpointService& service, const std::string& target) {
        std::string error;
        std::string text = service.diff(target, error);
        if (!error.empty())
            return {false, "", "git_checkpoint diff: " + error};
        const std::string label = target.empty() ? "latest" : target;
        if (text.empty())
            return {true, "checkpoint diff " + label + ": no changes", "", {{"target", label}}};

        bool truncated = false;
        if (text.size() > kMaxDiff) {
            text.resize(kMaxDiff);
            truncated = true;
        }
        std::string out = "checkpoint diff " + label + ":\n" + text;
        if (truncated)
            out += "\n[diff truncated]";
        return {true, out, "", {{"target", label}, {"truncated", truncated}}};
    }

    static ToolResult restore(CheckpointService& service, const std::string& target) {
        if (target.empty())
            return {false, "", "git_checkpoint restore: missing target (turn or SHA)"};
        const auto change = service.rollback(target);
        if (change.kind == CheckpointChangeKind::Failed)
            return {false, "", "git_checkpoint restore: " + change.error};

        std::string out = "restored " + checkpoint_describe(change.target);
        if (!change.checkpoint.id.empty())
            out += "; recorded revert " + checkpoint_describe(change.checkpoint);
        if (!change.error.empty())
            return {false, out, "git_checkpoint restore: " + change.error};
        return {true, out, "", {{"target", change.target.id}, {"revert", change.checkpoint.id}}};
    }
};

std::unique_ptr<Tool> make_checkpoint_tool() {
    return std::make_unique<CheckpointTool>();
}

} // namespace agent
