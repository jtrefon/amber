#ifndef AMBER_AGENT_SHA256_H
#define AMBER_AGENT_SHA256_H

#include <string>

namespace agent {

// Lowercase hex SHA-256 of `bytes`. Used to verify a well-known index's digest
// before anything is written to disk: an integrity check that cannot run must
// not silently pass, so the caller compares this against the advertised value.
std::string sha256_hex(const std::string& bytes);

} // namespace agent

#endif // AMBER_AGENT_SHA256_H
