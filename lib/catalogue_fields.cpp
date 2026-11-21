#include "agent/catalogue_fields.h"

#include <iterator>

namespace agent::catalogue {

// The order in each table is the precedence order the dialects already used;
// moving a provider's spelling into the table must not change which value
// wins when two spellings are present.
const char* const kContextWindow[] = {
    "meta.n_ctx",    "n_ctx",          "context_length", "top_provider.context_length",
    "limit.context", "context_window", "inputTokenLimit"};
const std::size_t kContextWindowCount = std::size(kContextWindow);

const char* const kContextTrain[] = {
    "meta.n_ctx_train", "n_ctx_train",    "context_length", "top_provider.context_length",
    "limit.context",    "context_window", "inputTokenLimit"};
const std::size_t kContextTrainCount = std::size(kContextTrain);

const char* const kMaxOutput[] = {"top_provider.max_completion_tokens", "max_completion_tokens",
                                  "limit.output", "max_output_tokens", "outputTokenLimit"};
const std::size_t kMaxOutputCount = std::size(kMaxOutput);

const char* const kInputModalities[] = {"input_modalities", "architecture.input_modalities",
                                        "modalities.input"};
const std::size_t kInputModalitiesCount = std::size(kInputModalities);

const char* const kOutputModalities[] = {"output_modalities", "architecture.output_modalities",
                                         "modalities.output"};
const std::size_t kOutputModalitiesCount = std::size(kOutputModalities);

const char* const kVerbosityLevels[] = {"verbosity.supported_levels", "verbosity.levels",
                                        "verbosity"};
const std::size_t kVerbosityLevelsCount = std::size(kVerbosityLevels);

const char* const kVerbosityDefault[] = {"verbosity.default_level", "verbosity.default",
                                         "default_verbosity"};
const std::size_t kVerbosityDefaultCount = std::size(kVerbosityDefault);

const char* const kThinking[] = {"thinking", "reasoning", "capabilities.thinking",
                                 "capabilities.reasoning"};
const std::size_t kThinkingCount = std::size(kThinking);

const char* const kEffortLevels[] = {"supported_levels", "supported_efforts", "levels", "effort"};
const std::size_t kEffortLevelsCount = std::size(kEffortLevels);

const char* const kEffortDefault[] = {"default_level", "default_effort", "default"};
const std::size_t kEffortDefaultCount = std::size(kEffortDefault);

const json* object_at(const json& obj, const char* dotted_path) {
    const json* cur = &obj;
    std::string part;
    for (const char* p = dotted_path;; ++p) {
        if (*p != '.' && *p != '\0') {
            part += *p;
            continue;
        }
        if (!cur->is_object())
            return nullptr;
        const auto it = cur->find(part);
        if (it == cur->end())
            return nullptr;
        cur = &(*it);
        part.clear();
        if (*p == '\0')
            return cur;
    }
}

const json* first_object(const json& obj, const char* const* paths, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const json* found = object_at(obj, paths[i]);
        if (found && found->is_object())
            return found;
    }
    return nullptr;
}

int first_int(const json& obj, const char* const* paths, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const json* found = object_at(obj, paths[i]);
        if (found && found->is_number()) {
            const int value = found->get<int>();
            if (value > 0)
                return value;
        }
    }
    return 0;
}

std::string first_string(const json& obj, const char* const* paths, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const json* found = object_at(obj, paths[i]);
        if (!found || !found->is_string())
            continue;
        const std::string value = found->get<std::string>();
        if (!value.empty())
            return value;
    }
    return {};
}

std::vector<std::string> strings_at(const json& obj, const char* dotted_path) {
    const json* found = object_at(obj, dotted_path);
    std::vector<std::string> out;
    if (!found)
        return out;
    if (found->is_string()) {
        const std::string one = found->get<std::string>();
        if (!one.empty())
            out.push_back(one);
    } else if (found->is_array()) {
        for (const auto& item : *found) {
            if (!item.is_string())
                continue;
            const std::string value = item.get<std::string>();
            if (!value.empty())
                out.push_back(value);
        }
    }
    return out;
}

std::vector<std::string> first_strings(const json& obj, const char* const* paths,
                                       std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<std::string> out = strings_at(obj, paths[i]);
        if (!out.empty())
            return out;
    }
    return {};
}

} // namespace agent::catalogue
