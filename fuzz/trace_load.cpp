// Fuzz the recorded-provider-trace validator: a trace is untrusted JSON that
// decides which request shape replays and which bytes go back. Parsing it must
// never crash, hang or read out of bounds, whatever the document contains.
#include <cstdint>
#include <string>

#include "bench/trace_store.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    agent::json document = agent::json::parse(text, nullptr, false);
    if (document.is_discarded())
        return 0;
    std::string error;
    try {
        (void)bench::parse_trace(document, "fuzz", error);
    } catch (...) {
        // The validator must report, not throw; a throw is still a finding for
        // the sanitizer build to surface through the corpus, but libFuzzer
        // should keep exploring instead of aborting on an uncaught exception.
    }
    return 0;
}
