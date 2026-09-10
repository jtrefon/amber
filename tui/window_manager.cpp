#include "window_manager.h"
#include "window.h"

#include <agent.h>
#include <agent/compressor.h>
#include <agent/plugin_runtime.h>
#include <agent/experience.h>
#include <agent/workspace.h>

#include <utility>

namespace tui {

WindowManager::WindowManager(agent::Config& cfg, agent::ToolRegistry& reg,
                             agent::PluginRuntime* plugin_runtime)
    : cfg_(cfg), reg_(reg), plugin_runtime_(plugin_runtime) {}

Window& WindowManager::new_window(const std::string& title) {
    auto w = std::make_unique<Window>();
    w->id = next_id_++;
    w->title = title;
    // The gate reads the LIVE config on every check (threshold_exceeded uses
    // agent_cfg.context_size), so it is safe to build from the current cfg:
    // a late /set model or an autodetect window reaches the gate through the
    // agent's cfg_ the next time Agent::resolve_window runs.
    auto comp_cfg = agent::load_compression_config(cfg_);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto exp_cfg = agent::load_experience_config(cfg_);
    auto mem_store = agent::make_memory_store(exp_cfg);
    auto retriever = std::make_unique<agent::MemoryRetriever>(*mem_store);
    agent::Agent a(cfg_, reg_, agent::AgentHooks{}, std::move(compressor),
                   std::move(gate), std::move(mem_store),
                   std::move(retriever), {}, {}, true);
    w->agent = std::make_unique<agent::Agent>(std::move(a));
    w->agent->policy().init(agent::Workspace::local_dir() + "/policy.json");
    // Attach the harness bus so plugins observe this window's turns. Without a
    // runtime (tests, headless hosts) the agent simply publishes nothing.
    if (plugin_runtime_) w->agent->set_events(plugin_runtime_->events());
    windows_.push_back(std::move(w));
    active_ = windows_.size() - 1;
    return *windows_.back();
}

Window& WindowManager::open_welcome_window() {
    auto w = std::make_unique<Window>();
    w->id = next_id_++;
    w->title = "amber";
    w->read_only = true;
    w->welcome_art = true;
    windows_.push_back(std::move(w));
    active_ = windows_.size() - 1;
    return *windows_.back();
}

Window& WindowManager::ensure_chat_window() {
    if (!win().read_only) return win();
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (!windows_[i]->read_only) {
            active_ = i;
            return win();
        }
    }
    return new_window("chat");
}

Window& WindowManager::win() { return *windows_[active_]; }
const Window& WindowManager::win() const { return *windows_[active_]; }

Window* WindowManager::by_id(size_t id) {
    for (auto& w : windows_)
        if (w && w->id == id) return w.get();
    return nullptr;
}

} // namespace tui
