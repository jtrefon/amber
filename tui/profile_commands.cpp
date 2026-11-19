#include "tui/profile_commands.h"

#include "agent/config.h"
#include "agent/profiles.h"
#include "agent/providers.h"
#include "tui/confirm_panel.h"
#include "tui/provider_form.h"
#include "tui/slash_dispatcher.h"
#include "tui/tui.h"

#include <filesystem>
#include <utility>

#include <nlohmann/json.hpp>

namespace tui {

namespace {

std::string model_or_auto(const agent::Profile& profile) {
    return profile.model.empty() ? std::string("auto") : profile.model;
}

// `load_profile` collapses "absent" and "malformed" into nullopt; the command
// surface can tell the user which one it is.
std::string missing_profile_message(const std::string& name) {
    std::error_code ec;
    if (std::filesystem::exists(agent::profile_path(name), ec))
        return "profile " + name + " could not be loaded (check its JSON)";
    return "there is no profile named " + name + " - /profile list shows the names";
}

std::string profile_row(const agent::Profile& profile, bool active, bool saved) {
    std::string row = active ? "> " : "  ";
    row += profile.name;
    row += "  " + profile.provider_name + " / " + model_or_auto(profile);
    row += "  " + agent::mask_key(profile.api_key);
    if (!saved)
        row += "  (not saved - /profile save)";
    return row;
}

} // namespace

void ProfileSurface::register_actions(SlashDispatcher& dispatcher) {
    dispatcher_ = &dispatcher;
    dispatcher.register_action("core.profile", [this](const std::string& a) { show(a); });
    dispatcher.register_action("core.profile.list", [this](const std::string&) { list(); });
    dispatcher.register_action("core.profile.add", [this](const std::string& a) { add(a); });
    dispatcher.register_action("core.profile.save", [this](const std::string&) { save(); });
    dispatcher.register_action("core.profile.switch",
                               [this](const std::string& a) { switch_to(a); });
    dispatcher.register_action("core.profile.show", [this](const std::string& a) { show(a); });
    dispatcher.register_action("core.profile.remove", [this](const std::string& a) { remove(a); });
}

void ProfileSurface::refresh_feed() {
    nlohmann::json subtree = nlohmann::json::object();
    auto& root = subtree["profile"]["children"];
    const auto profiles = agent::load_profiles();
    for (const auto& saved : profiles) {
        const bool active = saved.name == ui_.cfg_.profile_name;
        const agent::Profile profile =
            active ? agent::profile_from_config(ui_.cfg_, saved.name) : saved;
        for (const std::string verb : {"switch", "show", "remove"}) {
            const std::string action = "core.profile." + verb + "." + profile.name;
            nlohmann::json& leaf = root[verb]["children"][profile.name];
            leaf["action"] = action;
            if (verb == "switch")
                leaf["help"] =
                    active ? "active profile"
                           : "switch to " + model_or_auto(profile) + " on " + profile.provider_name;
            else if (verb == "show")
                leaf["help"] = "show " + profile.name + " (" + profile.provider_name + ")";
            else
                leaf["help"] = "remove " + profile.name;
            ui_.register_action(action, [this, name = profile.name, verb](const std::string&) {
                if (verb == "switch")
                    switch_to(name);
                else if (verb == "show")
                    show(name);
                else
                    remove(name);
            });
        }
    }
    ui_.settings_.merge_completions_json(subtree);
}

void ProfileSurface::show(const std::string& name) {
    const std::string target = name.empty() ? ui_.cfg_.profile_name : name;
    std::string why;
    if (!agent::valid_profile_name(target, &why)) {
        ui_.append_line(P_STATUS, "profile: " + why);
        return;
    }
    const bool live = target == ui_.cfg_.profile_name;
    const auto saved = agent::load_profile(target);
    agent::Profile profile;
    if (live)
        profile = agent::profile_from_config(ui_.cfg_, target);
    else if (saved)
        profile = *saved;
    else {
        ui_.append_line(P_STATUS, "profile: " + missing_profile_message(target));
        return;
    }
    ui_.append_line(P_STATUS, "profile: " + target + (live ? " (active)" : ""));
    ui_.append_line(P_STATUS, "  provider: " + profile.provider_name + " (" + profile.flavor + ")");
    ui_.append_line(P_STATUS,
                    "  endpoint: " + (profile.api_base.empty() ? "(unset)" : profile.api_base));
    ui_.append_line(P_STATUS, "  model: " + model_or_auto(profile));
    ui_.append_line(P_STATUS, "  key: " + agent::mask_key(profile.api_key));
    ui_.append_line(P_STATUS, "  temperature: " + std::to_string(profile.temperature) +
                                  "  max_tokens: " + std::to_string(profile.max_tokens) +
                                  "  thinking: " + profile.thinking);
    if (live && !saved)
        ui_.append_line(P_STATUS, "  (not saved - /profile save writes this connection)");
}

void ProfileSurface::list() {
    const auto profiles = agent::load_profiles();
    bool active_listed = false;
    for (const auto& saved : profiles) {
        const bool active = saved.name == ui_.cfg_.profile_name;
        active_listed = active_listed || active;
        // The active row is the live connection, not the last snapshot: /set
        // model and /set temperature do not rewrite every profile file.
        const agent::Profile profile =
            active ? agent::profile_from_config(ui_.cfg_, saved.name) : saved;
        ui_.append_line(P_STATUS, profile_row(profile, active, true));
    }
    if (!active_listed && agent::valid_profile_name(ui_.cfg_.profile_name)) {
        const agent::Profile live = agent::profile_from_config(ui_.cfg_, ui_.cfg_.profile_name);
        ui_.append_line(P_STATUS, profile_row(live, true, false));
    }
    if (profiles.empty() && !active_listed)
        ui_.append_line(P_STATUS, "no profiles - /profile add <name> creates one");
}

void ProfileSurface::add(const std::string& name) {
    if (name.empty()) {
        ui_.append_line(P_STATUS, "usage: /profile add <name>");
        return;
    }
    std::string why;
    if (!agent::valid_profile_name(name, &why)) {
        ui_.append_line(P_STATUS, "profile: " + why);
        return;
    }
    if (agent::load_profile(name)) {
        ui_.append_line(P_STATUS, "profile: " + name + " already exists - /profile switch " + name +
                                      " then /profile save updates it");
        return;
    }
    agent::Config seed = ui_.cfg_;
    if (!edit_provider_form(seed, "New profile " + name))
        return;
    const agent::Profile profile = agent::profile_from_config(seed, name);
    std::string error;
    if (!agent::save_profile(profile, &error)) {
        ui_.append_line(P_STATUS, "profile: " + error);
        return;
    }
    refresh_feed();
    ui_.append_line(P_STATUS, "profile " + name + " created (" + profile.provider_name + ", " +
                                  model_or_auto(profile) + ") - /profile switch " + name +
                                  " to use it");
    ui_.draw();
}

void ProfileSurface::save() {
    const std::string name = ui_.cfg_.profile_name;
    std::string why;
    if (!agent::valid_profile_name(name, &why)) {
        ui_.append_line(P_STATUS, "profile: " + why);
        return;
    }
    const agent::Profile profile = agent::profile_from_config(ui_.cfg_, name);
    std::string error;
    if (!agent::save_profile(profile, &error)) {
        ui_.append_line(P_STATUS, "profile: " + error);
        return;
    }
    // The global config is the connection every host starts from (the default
    // profile is not re-applied over it), so a save updates both.
    ui_.cfg_.save_global(agent::global_config_path());
    refresh_feed();
    ui_.append_line(P_STATUS, "profile " + name + " saved (" + profile.provider_name + ", " +
                                  model_or_auto(profile) + ")");
    ui_.draw();
}

void ProfileSurface::switch_to(const std::string& name) {
    if (name.empty()) {
        ui_.append_line(P_STATUS, "usage: /profile switch <name>");
        return;
    }
    std::string why;
    if (!agent::valid_profile_name(name, &why)) {
        ui_.append_line(P_STATUS, "profile: " + why);
        return;
    }
    if (!agent::load_profile(name)) {
        ui_.append_line(P_STATUS, "profile: " + missing_profile_message(name));
        return;
    }
    // A running turn already holds its client; replacing it mid-flight is the
    // race the provider switch has always refused. The queued switch lands at
    // the run slot's idle boundary, before the next turn.
    if (ui_.runs_.busy(ui_.win().id)) {
        pending_ = name;
        ui_.append_line(P_STATUS, "profile: switch to " + name +
                                      " queued - the running turn keeps " + ui_.cfg_.profile_name);
        ui_.draw();
        return;
    }
    apply(name);
    ui_.draw();
}

void ProfileSurface::apply(const std::string& name) {
    const auto profile = agent::load_profile(name);
    if (!profile) {
        ui_.append_line(P_STATUS, "profile: " + missing_profile_message(name));
        return;
    }
    agent::apply_profile(ui_.cfg_, *profile);
    // A pin measured against the outgoing model does not follow the profile.
    // The TUI template is already cleared; the window agent holds its own copy.
    if (ui_.win().agent)
        ui_.win().agent->clear_context_pin();
    // The profile may name a provider whose definition/catalogue was never
    // loaded in this process; the service is rebuilt against the new connection.
    ui_.providers_ = agent::make_default_provider_service(ui_.cfg_);
    if (dispatcher_)
        dispatcher_->land_provider_switch();
    refresh_feed();
    ui_.append_line(P_STATUS, "profile: " + name + " (" + profile->provider_name + ", " +
                                  model_or_auto(*profile) + ")");
}

void ProfileSurface::remove(const std::string& name) {
    if (name.empty()) {
        ui_.append_line(P_STATUS, "usage: /profile remove <name>");
        return;
    }
    std::string why;
    if (!agent::valid_profile_name(name, &why)) {
        ui_.append_line(P_STATUS, "profile: " + why);
        return;
    }
    if (!agent::load_profile(name)) {
        ui_.append_line(P_STATUS, "profile: " + missing_profile_message(name));
        return;
    }
    if (name == ui_.cfg_.profile_name) {
        if (name == "default") {
            ui_.append_line(P_STATUS,
                            "profile: the active default profile cannot be removed - switch to "
                            "another profile first");
            return;
        }
        ConfirmPanel confirm("Remove profile",
                             "\"" + name + "\" is active. Switch to default and remove it?");
        if (!confirm.run())
            return;
        if (!agent::load_profile("default")) {
            const agent::Profile fallback = agent::profile_from_config(ui_.cfg_, "default");
            (void)agent::save_profile(fallback);
        }
        switch_to("default");
    }
    std::string error;
    if (!agent::remove_profile(name, &error)) {
        ui_.append_line(P_STATUS, "profile: " + error);
        return;
    }
    refresh_feed();
    ui_.append_line(P_STATUS, "profile " + name + " removed");
    ui_.draw();
}

void ProfileSurface::switch_project(const std::string& root) {
    const std::string name = agent::project_profile_name(root);
    if (name.empty() || name == ui_.cfg_.profile_name)
        return;
    switch_to(name);
}

void ProfileSurface::apply_pending() {
    if (pending_.empty() || ui_.runs_.busy(ui_.win().id))
        return;
    const std::string name = std::exchange(pending_, {});
    apply(name);
    ui_.draw();
}

} // namespace tui
