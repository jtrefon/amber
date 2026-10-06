#pragma once

#include <string>
#include <vector>

namespace tui {

// Decodes a bracketed-paste byte stream: the terminals send
// ESC [ 2 0 0 ~  <text>  ESC [ 2 0 1 ~  when something is pasted into a field.
//
// This exists because ESC is a real key in every modal dialog. A paste begins
// with ESC, so without decoding it the first byte of every paste was read as
// "cancel" and the dialog closed -- which is why pasting an API key appeared to
// be blocked, and why a 60-character token had to be typed by hand.
//
// Pure: it consumes bytes and says what happened, so the sequence handling is
// testable without a terminal, a form, or a clipboard.
class BracketPasteDecoder {
public:
    enum class Event {
        None,     // consumed as part of a marker; nothing to insert
        NotPaste, // not a paste byte; hand it back to the normal key path
        Begin,    // the paste-start marker was seen
        Text,     // one character of pasted content
        End,      // the paste-end marker was seen
    };

    // Feed one byte. `out` is set for Event::Text.
    Event feed(int ch, char& out);

    // True once Begin has been seen and before End, so the caller knows to
    // insert rather than route the byte as a keystroke.
    bool pasting() const noexcept { return pasting_; }

    // Bytes read but not consumed by a paste (the prefix of a sequence that
    // turned out not to be one). The caller pushes these back so a genuine ESC
    // still cancels. Ordered as read.
    const std::vector<int>& unconsumed() const noexcept { return unconsumed_; }

    // Drop any partial sequence. Called when a key that is not part of a paste
    // arrives, or when the loop ends.
    void reset() noexcept;

private:
    // State only; reset() additionally drops the pushback list, which is why the
    // mismatch path below uses this one -- it has to record the bytes BEFORE
    // clearing, or they would be lost with the state.
    void clear_state() noexcept;
    Event feed_inside(int ch, char& out);
    Event feed_start(int ch);

    // 0 = idle, 1 = matching the start marker, 2 = inside, 3 = matching the end
    int state_ = 0;
    std::size_t pos_ = 0;
    bool pasting_ = false;
    std::vector<int> unconsumed_;
};

} // namespace tui