#ifndef AGENT_JSON_READ_H
#define AGENT_JSON_READ_H

#include <nlohmann/json.hpp>
#include <exception>
#include <string>

namespace agent {

// Untrusted payloads arrive typed however the model, an MCP server or a plugin
// chose to type them: `true`, 1, "true". Adopt any reasonable spelling rather
// than throwing — one coerced field must never cost a turn.
inline bool read_bool(const nlohmann::json& obj, const char* key, bool fallback) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_boolean())
        return it->get<bool>();
    if (it->is_number())
        return it->get<double>() != 0.0;
    if (it->is_string()) {
        const std::string v = it->get<std::string>();
        if (v == "true" || v == "1")
            return true;
        if (v == "false" || v == "0" || v.empty())
            return false;
    }
    return fallback;
}

inline std::string read_string(const nlohmann::json& obj, const char* key, const char* fallback) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number() || it->is_boolean())
        return it->dump();
    return fallback;
}

// Integers arrive typed however the sender chose: 60, "60", true. A numerical
// string is adopted only when it is entirely numeric — "60s" is a duration, not
// a number, and guessing at it would be worse than the fallback.
inline long long read_int(const nlohmann::json& obj, const char* key, long long fallback) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_number())
        return static_cast<long long>(it->get<double>());
    if (it->is_boolean())
        return it->get<bool>() ? 1 : 0;
    if (it->is_string()) {
        const std::string v = it->get<std::string>();
        try {
            std::size_t used = 0;
            const double d = std::stod(v, &used);
            if (used == v.size())
                return static_cast<long long>(d);
        } catch (const std::exception&) {
        }
    }
    return fallback;
}

// Nested reads: a container field of the wrong type must not throw before the
// value inside it can be guarded, so an object/array field that is absent or
// typed as something else reads as empty.
inline nlohmann::json read_object(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_object() ? *it : nlohmann::json::object();
}

inline nlohmann::json read_array(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_array() ? *it : nlohmann::json::array();
}

} // namespace agent

#endif
