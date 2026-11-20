#include "agent/profiles.h"

#include "agent/llm.h"
#include "agent/private_file.h"
#include "agent/workspace.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace agent {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

template <typename T> T read_field(const json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    if (it == j.end())
        return fallback;
    try {
        return it->get<T>();
    } catch (...) {
        return fallback;
    }
}

json profile_to_json(const Profile& profile) {
    json j;
    j["version"] = 1;
    j["name"] = profile.name;
    j["provider"] = profile.provider_name;
    j["flavor"] = profile.flavor;
    j["api_base"] = profile.api_base;
    j["api_key"] = profile.api_key;
    j["model"] = profile.model;
    j["temperature"] = profile.temperature;
    j["max_tokens"] = profile.max_tokens;
    j["thinking"] = profile.thinking;
    return j;
}

Profile profile_from_json(const std::string& name, const json& j) {
    Profile profile;
    profile.name = name;
    profile.provider_name = read_field(j, "provider", std::string("custom"));
    profile.flavor = read_field(j, "flavor", std::string("openai"));
    profile.api_base = read_field(j, "api_base", std::string{});
    profile.api_key = read_field(j, "api_key", std::string{});
    profile.model = read_field(j, "model", std::string{});
    profile.temperature = read_field(j, "temperature", 0.2);
    const long long max_tokens = read_field(j, "max_tokens", 16384LL);
    profile.max_tokens = max_tokens > 0 ? static_cast<size_t>(max_tokens) : 16384;
    const std::string thinking = read_field(j, "thinking", std::string("auto"));
    profile.thinking =
        (thinking == "on" || thinking == "off" || thinking == "auto") ? thinking : "auto";
    return profile;
}

} // namespace

bool valid_profile_name(const std::string& name, std::string* why) {
    auto refuse = [&](const std::string& reason) {
        if (why)
            *why = reason;
        return false;
    };
    if (name.empty())
        return refuse("a profile name cannot be empty");
    if (name.size() > 64)
        return refuse("a profile name is at most 64 characters");
    if (name == "." || name == "..")
        return refuse("a profile name cannot be '.' or '..'");
    for (unsigned char c : name) {
        const bool allowed = std::isalnum(c) || c == '.' || c == '_' || c == '-';
        if (!allowed)
            return refuse("a profile name is letters, digits, '.', '_' and '-'");
    }
    return true;
}

std::string mask_key(const std::string& key) {
    if (key.empty())
        return "(unset)";
    if (key.size() <= 8)
        return "****";
    return key.substr(0, 3) + "..." + key.substr(key.size() - 4);
}

Profile profile_from_config(const Config& cfg, const std::string& name) {
    Profile profile;
    profile.name = name;
    profile.provider_name = cfg.provider_name;
    profile.flavor = cfg.flavor;
    profile.api_base = cfg.api_base;
    profile.api_key = cfg.api_key;
    profile.model = cfg.model;
    profile.temperature = cfg.temperature;
    profile.max_tokens = cfg.max_tokens;
    profile.thinking = cfg.thinking;
    return profile;
}

void apply_profile(Config& cfg, const Profile& profile) {
    cfg.profile_name = profile.name.empty() ? "default" : profile.name;
    cfg.provider_name = profile.provider_name.empty() ? "custom" : profile.provider_name;
    cfg.flavor = profile.flavor.empty() ? "openai" : profile.flavor;
    cfg.api_base = profile.api_base;
    cfg.api_key = profile.api_key;
    // A profile is user configuration, not a project default: a workspace
    // amber.conf base layer must not redirect the endpoint it names.
    cfg.api_base_from_provider = !profile.api_base.empty();
    cfg.model = profile.model;
    cfg.model_explicit = !profile.model.empty();
    cfg.temperature = profile.temperature;
    cfg.max_tokens = profile.max_tokens;
    cfg.thinking = profile.thinking;
    // Both describe the outgoing model. A window measured against one endpoint
    // is how the gauge, the compression gate and the pin warning kept lying.
    cfg.context_size = 0;
    cfg.context_explicit = false;
    apply_model_capabilities(cfg, ModelCapabilities{});
    // Provider-scoped runtime state: a response id and a prompt count describe
    // the outgoing connection, and replaying either against a new one is a
    // stale-cache request at best.
    cfg.previous_response_id.clear();
    cfg.prompt_tokens_used = -1;
}

std::string profiles_dir() {
    return global_config_dir() + "/profiles";
}

std::string profile_path(const std::string& name) {
    return profiles_dir() + "/" + name + ".json";
}

std::optional<Profile> load_profile(const std::string& name) {
    if (!valid_profile_name(name))
        return std::nullopt;
    std::ifstream in(profile_path(name));
    if (!in)
        return std::nullopt;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;
    return profile_from_json(name, j);
}

std::vector<Profile> load_profiles() {
    std::vector<Profile> profiles;
    std::error_code ec;
    const fs::path dir = profiles_dir();
    if (!fs::is_directory(dir, ec))
        return profiles;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
            continue;
        const std::string name = entry.path().stem().string();
        if (!valid_profile_name(name))
            continue;
        if (auto profile = load_profile(name))
            profiles.push_back(std::move(*profile));
    }
    std::sort(profiles.begin(), profiles.end(),
              [](const Profile& a, const Profile& b) { return a.name < b.name; });
    return profiles;
}

bool save_profile(const Profile& profile, std::string* error) {
    std::string why;
    if (!valid_profile_name(profile.name, &why)) {
        if (error)
            *error = why;
        return false;
    }
    if (!ensure_private_dir(profiles_dir())) {
        if (error)
            *error = "cannot create " + profiles_dir();
        return false;
    }
    const std::string path = profile_path(profile.name);
    if (!write_private_file(path, profile_to_json(profile).dump(2) + "\n")) {
        if (error)
            *error = "cannot write " + path;
        return false;
    }
    return true;
}

bool remove_profile(const std::string& name, std::string* error) {
    std::string why;
    if (!valid_profile_name(name, &why)) {
        if (error)
            *error = why;
        return false;
    }
    std::error_code ec;
    const std::string path = profile_path(name);
    if (!fs::exists(path, ec)) {
        if (error)
            *error = "no profile named " + name;
        return false;
    }
    if (!fs::remove(path, ec) || ec) {
        if (error)
            *error = "could not remove " + path;
        return false;
    }
    return true;
}

bool ensure_default_profile(const Config& cfg) {
    std::error_code ec;
    if (fs::exists(profiles_dir(), ec))
        return false;
    return save_profile(profile_from_config(cfg, "default"));
}

std::string project_profile_name(const std::string& project_root) {
    const std::string root = project_root.empty() ? Workspace::project_root() : project_root;
    std::ifstream in(root + "/.amber/profile");
    if (!in)
        return {};
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        return trimmed;
    }
    return {};
}

bool apply_project_profile(Config& cfg, const std::string& project_root, std::string* note) {
    const std::string name = project_profile_name(project_root);
    if (name.empty())
        return false;
    std::string why;
    if (!valid_profile_name(name, &why)) {
        if (note)
            *note = "invalid .amber/profile: " + why;
        return false;
    }
    const auto profile = load_profile(name);
    if (!profile) {
        if (note)
            *note = "project profile '" + name + "' does not exist";
        return false;
    }
    apply_profile(cfg, *profile);
    if (note)
        *note = name + " (from .amber/profile)";
    return true;
}

} // namespace agent
