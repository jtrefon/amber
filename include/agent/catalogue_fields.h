#ifndef AGENT_CATALOGUE_FIELDS_H
#define AGENT_CATALOGUE_FIELDS_H

#include "agent/tool.h"

#include <cstddef>
#include <string>
#include <vector>

namespace agent::catalogue {

// Dotted field paths, ordered by precedence. A concept is a table entry, not
// a branch in a parser: a provider that publishes a new spelling becomes a new
// row here. Every reader skips wrong-typed values, so untrusted JSON is always
// "not advertised" rather than a crash or a guess.
extern const char* const kContextWindow[];
extern const std::size_t kContextWindowCount;
extern const char* const kContextTrain[];
extern const std::size_t kContextTrainCount;
extern const char* const kMaxOutput[];
extern const std::size_t kMaxOutputCount;
extern const char* const kInputModalities[];
extern const std::size_t kInputModalitiesCount;
extern const char* const kOutputModalities[];
extern const std::size_t kOutputModalitiesCount;
extern const char* const kVerbosityLevels[];
extern const std::size_t kVerbosityLevelsCount;
extern const char* const kVerbosityDefault[];
extern const std::size_t kVerbosityDefaultCount;
extern const char* const kThinking[];
extern const std::size_t kThinkingCount;
extern const char* const kEffortLevels[];
extern const std::size_t kEffortLevelsCount;
extern const char* const kEffortDefault[];
extern const std::size_t kEffortDefaultCount;

const json* object_at(const json& obj, const char* dotted_path);
const json* first_object(const json& obj, const char* const* paths, std::size_t count);
int first_int(const json& obj, const char* const* paths, std::size_t count);
std::string first_string(const json& obj, const char* const* paths, std::size_t count);
std::vector<std::string> first_strings(const json& obj, const char* const* paths,
                                       std::size_t count);
// Every string at one dotted path (a string is one entry, an array is many).
std::vector<std::string> strings_at(const json& obj, const char* dotted_path);

} // namespace agent::catalogue

#endif // AGENT_CATALOGUE_FIELDS_H
