#ifndef AGENT_DIALECT_GEMINI_H
#define AGENT_DIALECT_GEMINI_H

#include "agent/dialect.h"

#include <memory>

namespace agent {

// Google Gemini (generativelanguage.googleapis.com): `generateContent` /
// `streamGenerateContent`, `contents` + `parts`, `systemInstruction`,
// `functionDeclarations`, `usageMetadata`. Registered by a provider plugin;
// nothing in the transport, the agent loop, or the UI knows it exists.
std::unique_ptr<Dialect> make_gemini_dialect();

} // namespace agent

#endif // AGENT_DIALECT_GEMINI_H
