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

    // Bytes that were held back as a possible marker and then turned out to be
    // content: pasting the literal text "ESC [ 2 0 1" must insert all of it, not
    // swallow it. feed() emits at most one byte per call, so a rejected marker
    // (up to six bytes) is queued here instead.
    //
    // Drain these BEFORE every feed(): has_pending() is checked first, and
    // take_pending() returns one byte at a time. Feeding a new byte while bytes
    // are queued would drop that byte, because feed() has no way to return two.
    // consume_paste_burst() does this correctly; use it rather than feed().
    bool has_pending() const noexcept { return pending_at_ < pending_.size(); }

    // One queued byte as Event::Text, or Event::None when the queue is empty.
    Event take_pending(char& out);

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

    // Content rescued from a marker that was rejected. pending_at_ is a cursor
    // rather than erasing the front, so appending while a caller is mid-drain
    // stays ordered.
    std::vector<char> pending_;
    std::size_t pending_at_ = 0;
};

// What one burst of bytes meant, and what to insert.
//
// The decision is separated from the terminal because form_edit.cpp can only be
// driven by a real ncurses form: it cannot be unit-tested, and untestable ncurses
// glue is how a change like this lands with patch coverage at 48%. Consume the
// burst through this and the only thing left in the form is wgetch/ungetch.
struct PasteBurst {
    std::vector<char> insert; // pasted content, in order
    bool started = false;     // the start marker matched
};

// Feed a burst read straight after an ESC. `d` carries the ESC itself already
// fed, so the caller owns the first byte and this owns the rest.
//
// started == false means "not a paste": the bytes must be pushed back so a bare
// Esc still cancels.
PasteBurst consume_paste_burst(BracketPasteDecoder& d, const std::vector<int>& bytes);

} // namespace tui