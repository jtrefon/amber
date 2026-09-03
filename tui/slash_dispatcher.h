#ifndef AMBER_TUI_SLASH_DISPATCHER_H
#define AMBER_TUI_SLASH_DISPATCHER_H

#include <functional>
#include <string>
#include <vector>

#include <agent/agent.h>
#include <agent/model_probe.h>
#include <agent/plugin.h>

#include "action_registry.h"
#include "palette.h"

namespace tui {
class Tui;

class SlashDispatcher {
public:
    explicit SlashDispatcher(Tui& tui);
    const std::vector<palette::Command>& commands();
    void build_commands();
    const palette::Command* find_command(const std::string& name);
    std::string plugin_state_name(agent::PluginState st) const;
    bool handle_slash(const std::string& line);
    void register_action(const std::string& action,
                         std::function<void(const std::string&)> handler);
    void register_builtin_actions();
    bool busy_reject(const std::string& what);
    void request_quit();
    void refresh_completions();
    void refresh_model_list();
    void refresh_policy_feed();
    void refresh_provider_feed();
    void refresh_job_feed();
    double compression_threshold_effective() const;
    void cmd_model_set(const std::string& arg);
    void cmd_provider(const std::string& arg);
    void job_kill(const std::string& id);
    void job_read(const std::string& id);
    void apply_policy_rule(const std::string& name, const std::string& lvl);
    void show_policy_rule(const std::string& name);

    void cmd_set_detection_toggle(const std::string& key, const std::string& val);
    void cmd_set_subagent_parallel(const std::string& val);
    void cmd_set_subagent_max(const std::string& val);
    void cmd_get_subagent();
    void cmd_set_reasoning_effort(const std::string& val);
    void cmd_get_reasoning();
    void cmd_provider_list();
    void cmd_provider_delete(const std::string& name);
    void cmd_provider_test(const std::string& name);
    void cmd_session_load(const std::string& id);
    void cmd_session_delete(const std::string& id);
    void cmd_files_ls(const std::string& rest);
    void cmd_files_tree(const std::string& rest);
    void cmd_files_open(const std::string& rest);
    void cmd_files_find(const std::string& rest);
    void cmd_system_exec(const std::string& rest);
    void cmd_system_delete(const std::string& rest);
    void cmd_system_rmdir(const std::string& rest);
    void cmd_system_mkdir(const std::string& rest);
    void cmd_system_mv(const std::string& rest);
    void cmd_system_cp(const std::string& rest);
    void cmd_system_info(const std::string& rest);
    void cmd_system_ps();
    void cmd_system_kill(const std::string& rest);
    void cmd_system_df();
    void cmd_system_uptime();
    void cmd_system_uname();
    void cmd_skills_interop(const std::string& val);
    void cmd_skills_refresh();
    void cmd_skills_create(const std::string& args);
    void cmd_skills_delete(const std::string& args);
    void cmd_skills_install(const std::string& source);
    void cmd_skills_uninstall(const std::string& name);
    void cmd_get_config();
    void cmd_get_model();
    void cmd_get_model_list();
    void cmd_get_model_context();
    void cmd_get_provider();
    void cmd_get_policy(const std::string& arg);
    void cmd_get_policy_rule(const std::string& arg);
    void cmd_set_policy_rule(const std::string& arg);
    void cmd_get_policy_mode();
    void cmd_get_policy_approval();
    void cmd_get_policy_timeout();
    void cmd_get_display();
    void cmd_get_think();
    void cmd_get_detection(const std::string& sub);
    void cmd_get_compression();
    void cmd_window_new();
    void cmd_window_close();
    void cmd_window_list();
    void cmd_window_rename(const std::string& name);
    void cmd_mcp_show(const std::string& server);
    void cmd_mcp_connect(const std::string& server);
    void cmd_mcp_disconnect(const std::string& server);
    void cmd_mcp_refresh(const std::string& server);
    void cmd_mcp_prompts(const std::string& server);
    void cmd_mcp_set_enabled(const std::string& server, bool on);
    void cmd_mcp_trust(const std::string& args);
    void cmd_plugin_list();
    void cmd_plugin_status(const std::string& id);
    void cmd_plugin_info(const std::string& id);
    void cmd_plugin_enable(const std::string& id);
    void cmd_plugin_disable(const std::string& id);
    void cmd_plugin_get(const std::string& args);
    void cmd_plugin_set(const std::string& args);
    void cmd_plugin_install(const std::string& source);
    void cmd_plugin_uninstall(const std::string& id);
    std::string usage(const palette::Command& c) const;
    void cmd_help(const std::string& arg);
    void cmd_window(const std::string& arg);
    void cmd_job(const std::string& rest);
    void cmd_compress(const std::string& arg);
    void cmd_set(const std::string& arg);
    void cmd_get(const std::string& arg);
    void apply_compression_threshold(const std::string& v);
    void apply_compression_min_turns(const std::string& v);
    void apply_compression_target_pct(const std::string& v);
    void apply_compression_keep_last_prompts(const std::string& v);
    void cmd_skills_set(const std::string& rest);
    void cmd_skills_get(const std::string& sub);
    void cmd_mcp(const std::string& rest);
    void cmd_plugin(const std::string& rest);
    void cmd_prompt(const std::string& rest);
    void cmd_prompt_list();
    void job_ls();
    void job_start(const std::string& cmd);
    void build_settings();
    const std::vector<agent::ModelInfo>& model_info() const noexcept { return model_info_; }
    void set_model_info(std::vector<agent::ModelInfo> v) { model_info_ = std::move(v); }

private:
    Tui& tui_;
    std::vector<palette::Command> commands_;
    tui::ActionRegistry action_registry_;
    std::vector<agent::ModelInfo> model_info_;
};

} // namespace tui

#endif