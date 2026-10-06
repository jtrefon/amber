#include "tui/bracket_paste.h"
#include "tests/test_util.h"

namespace {

using Ev = tui::BracketPasteDecoder::Event;

// Feed a whole string, collecting the pasted text and the markers seen.
struct Fed {
    std::string text;
    int begins = 0;
    int ends = 0;
    bool ended_cleanly = false;
};

Fed feed_all(tui::BracketPasteDecoder& d, const std::string& bytes) {
    Fed f;
    for (const char c : bytes) {
        char out = 0;
        switch (d.feed(static_cast<unsigned char>(c), out)) {
        case Ev::Begin:
            ++f.begins;
            break;
        case Ev::Text:
            f.text += out;
            break;
        case Ev::End:
            ++f.ends;
            f.ended_cleanly = true;
            break;
        case Ev::NotPaste:
        case Ev::None:
            break;
        }
    }
    return f;
}

constexpr const char kPaste[] = "\033[200~sk-ant-0123456789\033[201~";

} // namespace

// The whole point: a paste must not be mistaken for the Esc that cancels the
// dialog. Before this existed, the leading ESC of every paste closed the form,
// which is why an API key had to be typed by hand.
TEST(bracket_paste_decodes_a_pasted_token) {
    tui::BracketPasteDecoder d;
    auto f = feed_all(d, kPaste);
    ASSERT_EQ(f.begins, 1);
    ASSERT_EQ(f.ends, 1);
    ASSERT_TRUE(f.ended_cleanly);
    ASSERT_EQ(f.text, std::string("sk-ant-0123456789"));
}

TEST(bracket_paste_is_in_pasting_state_only_between_the_markers) {
    tui::BracketPasteDecoder d;
    ASSERT_FALSE(d.pasting());
    feed_all(d, "\033[200~");
    ASSERT_TRUE(d.pasting());
    feed_all(d, "\033[201~");
    ASSERT_FALSE(d.pasting());
}

// ESC '[' is the first two bytes of the start marker, so the decoder cannot
// call it either way -- it keeps matching. Disambiguating a lone Esc is the
// CALLER's job and needs a non-blocking read: if no byte follows immediately,
// Esc is a key; if one does, this is a paste. Arrow keys are unaffected because
// ncurses reports them as KEY_UP, not as an ESC prefix.
TEST(bracket_paste_escape_bracket_is_held_as_a_possible_paste_prefix) {
    tui::BracketPasteDecoder d;
    char out = 0;
    ASSERT_TRUE(d.feed(27, out) == Ev::None);
    ASSERT_TRUE(d.feed('[', out) == Ev::None);
    ASSERT_FALSE(d.pasting());
    ASSERT(d.unconsumed().empty()); // still a candidate, nothing pushed back
}

TEST(bracket_paste_escape_followed_by_a_key_is_not_a_paste) {
    tui::BracketPasteDecoder d;
    auto f = feed_all(d, "\033[Ax");
    ASSERT_EQ(f.begins, 0);
    ASSERT_EQ(f.text, std::string(""));
    ASSERT_EQ(d.unconsumed().size(), 2u); // ESC and '[', pushed back
}

// An ordinary key is passed straight through and must not leave the decoder in
// a half-matched state.
TEST(bracket_paste_plain_characters_are_untouched) {
    tui::BracketPasteDecoder d;
    char out = 0;
    ASSERT(d.feed('a', out) == Ev::NotPaste);
    ASSERT(d.feed(27, out) == Ev::None);
    // 'x' does not continue the marker, so only the ESC is handed back -- 'x'
    // was never a matched prefix byte and is handled as an ordinary key.
    ASSERT(d.feed('x', out) == Ev::NotPaste);
    ASSERT_EQ(d.unconsumed().size(), 1u);
    ASSERT_EQ(d.unconsumed()[0], 27);
}

// A partially matched marker followed by a reset must not leak into the next
// key: the decoder has to be reusable after a cancelled paste.
TEST(bracket_paste_pushback_is_cleared_by_reset) {
    tui::BracketPasteDecoder d;
    char out = 0;
    d.feed(27, out);
    d.feed('[', out);
    d.feed('A', out);
    ASSERT_EQ(d.unconsumed().size(), 2u);
    d.reset();
    ASSERT(d.unconsumed().empty());
    ASSERT_FALSE(d.pasting());
}

// A partial match then a cancel must recover cleanly.
TEST(bracket_paste_reset_after_a_partial_marker) {
    tui::BracketPasteDecoder d;
    char out = 0;
    d.feed(27, out);
    d.feed('[', out);
    d.feed('2', out);
    d.reset();
    ASSERT_FALSE(d.pasting());
    ASSERT(d.unconsumed().empty());
    // usable again
    auto f = feed_all(d, kPaste);
    ASSERT_EQ(f.begins, 1);
    ASSERT_EQ(f.text, std::string("sk-ant-0123456789"));
}

// Multi-line content is still inserted; the caller decides what a field does
// with it, and a single-line field simply keeps the trailing part.
TEST(bracket_paste_passes_every_byte_of_content_through) {
    tui::BracketPasteDecoder d;
    auto f = feed_all(d, "\033[200~line1\nline2\033[201~");
    ASSERT_EQ(f.text, std::string("line1\nline2"));
}

// The end marker's own bytes must not leak into the text, even when the content
// happens to start with ESC [ 201-like bytes.
TEST(bracket_paste_end_marker_bytes_do_not_leak) {
    tui::BracketPasteDecoder d;
    auto f = feed_all(d, "\033[200~abc\033[201~def");
    ASSERT_EQ(f.text, std::string("abc"));
    ASSERT_EQ(f.ends, 1);
}

// Empty paste: markers only, no content, no crash.
TEST(bracket_paste_handles_an_empty_paste) {
    tui::BracketPasteDecoder d;
    auto f = feed_all(d, "\033[200~\033[201~");
    ASSERT_EQ(f.begins, 1);
    ASSERT_EQ(f.ends, 1);
    ASSERT_EQ(f.text, std::string(""));
}