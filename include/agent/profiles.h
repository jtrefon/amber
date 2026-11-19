#ifndef AGENT_PROFILES_H
#define AGENT_PROFILES_H

#include <optional>
#include <string>
#include <vector>

#include "agent/config.h"

namespace agent {

// A named provider connection: the account, endpoint and model a consultant
// switches between without editing config or restarting. A profile owns the
// connection only; mode, UI, tools and project state stay where they were.
struct Profile {
    std::string name = "default";
    std::string provider_name = "custom";
    std::string flavor = "openai";
    std::string api_base;
    std::string api_key;
    std::string model;
    double temperature = 0.2;
    size_t max_tokens = 16384;
    std::string thinking = "auto";
};

// A profile name is a file stem: letters, digits, '.', '_' and '-', at most 64
// characters, and never "." or "..". A name that can escape the profiles
// directory is refused before it reaches a path.
bool valid_profile_name(const std::string& name, std::string* why = nullptr);

// "sk-...abcd"; a short or empty key never appears in a readout.
std::string mask_key(const std::string& key);

// The profile a live Config would save: the connection fields only.
Profile profile_from_config(const Config& cfg, const std::string& name);

// Overlay a profile's connection onto a Config. The context window and the
// model capabilities are cleared: both describe the outgoing model, and a pin
// carried across is how the gauge and the compression gate kept lying.
void apply_profile(Config& cfg, const Profile& profile);

// ~/.config/amber/profiles and <name>.json.
std::string profiles_dir();
std::string profile_path(const std::string& name);

// Every valid profile file, sorted by name. A malformed file is skipped rather
// than failing the whole list.
std::vector<Profile> load_profiles();
std::optional<Profile> load_profile(const std::string& name);

// Atomically write a profile (0600) and delete one. Errors are returned rather
// than thrown: a command reports them in the scrollback.
bool save_profile(const Profile& profile, std::string* error = nullptr);
bool remove_profile(const std::string& name, std::string* error = nullptr);

// First-run migration: when the profiles directory does not exist, capture the
// current connection as `default`. A directory that exists is never touched, so
// a deliberately edited default survives every later start. Returns true when
// it created the profile.
bool ensure_default_profile(const Config& cfg);

// The profile name in <project>/.amber/profile, or empty when there is none.
// The file contains just the name; blank lines and '#' comments are ignored.
std::string project_profile_name(const std::string& project_root);

// Apply the project override, if one exists. `note` receives a one-line
// explanation when it switched, or why it could not. Returns true when the
// config now uses the project's profile.
bool apply_project_profile(Config& cfg, const std::string& project_root,
                           std::string* note = nullptr);

} // namespace agent

#endif // AGENT_PROFILES_H
