// Fuzz the Markdown renderer.
//
// Model output reaches this as raw bytes: a fenced block, a table, a list, a
// thousand-deep quote, a half-written UTF-8 sequence at the end of a stream. The unit
// suite covers the constructs we care about rendering; libFuzzer covers the nesting
// and byte-boundary shapes nobody thought to write down.

#include <cstddef>
#include <cstdint>
#include <string>

#include "tui/markdown.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string markdown(reinterpret_cast<const char*>(data), size);
    const std::vector<tui::rich::Line> lines = tui::md::render(markdown);

    // Read the output rather than discarding it, so it cannot be optimized away and so
    // any out-of-range access while rendering surfaces here.
    for (const tui::rich::Line& line : lines) {
        (void)line.runs.size();
        (void)line.is_code;
        (void)line.is_hr;
        (void)line.heading;
    }
    return 0;
}
