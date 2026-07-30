
#ifndef AGENT_REQUEST_BUILDER_H
#define AGENT_REQUEST_BUILDER_H

#include "agent/config.h"
#include "agent/llm.h"
#include <nlohmann/json.hpp>
#include <vector>

namespace agent {

// Builds the OpenAI-compatible /chat/completions JSON request body from a
// Config, the message history, and the tool set. Kept separate from the
// transport so the wire format is unit-testable without libcurl.
json build_chat_body(const Config& cfg, const std::vector<Message>& messages,
                     const std::vector<Tool*>& tools, bool stream);

} // namespace agent

#endif // AGENT_REQUEST_BUILDER_H
