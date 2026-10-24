#ifndef AGENT_REQUEST_FAILURE_H
#define AGENT_REQUEST_FAILURE_H

#include <cstdint>
#include <string>

namespace agent {

// Classify a non-retryable HTTP 4xx: is the failure in OUR request, and can the
// request be repaired by adaptation?
//
// This lives in its own header because both the failure classifier
// (agent_helpers.h) and the repairs that answer it (repair.h) need it, and
// agent_helpers.h includes agent.h -- including it from repair.h would close a
// cycle that leaves the enum undefined at the point of use.
enum class RequestFailure : std::uint8_t {
    None,           // not a known recoverable request fault
    ModelName,      // server rejected the model id (vLLM-style "does not exist")
    TemplateParser, // server cannot parse the tool grammar for the loaded
                    // template (llama.cpp "Unable to generate parser ...")
    Auth            // API key/token rejected (HTTP 401/403)
};

RequestFailure classify_request_failure(const std::string& error_text);

} // namespace agent

#endif // AGENT_REQUEST_FAILURE_H
