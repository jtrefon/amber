#ifndef AMBER_TUI_CACHE_NOTE_H
#define AMBER_TUI_CACHE_NOTE_H

#include "agent/llm.h"

#include <string>

namespace tui {

// One sentence about the prompt cache, for `/get provider`.
//
// The failure this exists for is silent: a prefix under the model's minimum is
// cached not at all -- the API accepts the breakpoint, returns no error, and
// reports 0 read / 0 write, which is exactly what a client that sent no
// breakpoint produces. Without a readout, "caching isn't working" has no
// evidence anywhere in amber, and the only place left to look is the provider's
// dashboard.
inline std::string prompt_cache_note(const std::string& model, const std::string& ttl,
                                     long prefix_tokens, long cache_read, long cache_write) {
    std::string line = "prompt cache: " + ttl + " · last prefix ";
    if (prefix_tokens <= 0)
        return line + "not measured yet (one request fills this in)";
    line += std::to_string(prefix_tokens) + " tokens";
    const int minimum = agent::min_cacheable_tokens(model);
    if (cache_read == 0 && cache_write == 0 && minimum > 0 && prefix_tokens < minimum)
        return line + " · below the " + std::to_string(minimum) + "-token minimum for " + model +
               ": nothing is cached";
    if (cache_read == 0 && cache_write == 0)
        return line + " · the provider has reported no cache at all";
    if (cache_read == 0)
        return line + " · written, not read back yet";
    return line + " · reading from cache";
}

} // namespace tui

#endif // AMBER_TUI_CACHE_NOTE_H
