#include "tui/bracket_paste.h"

namespace tui {

namespace {

// ESC [ 2 0 0 ~  and  ESC [ 2 0 1 ~
constexpr const char kStart[] = "\033[200~";
constexpr const char kEnd[] = "\033[201~";
constexpr std::size_t kStartLen = sizeof(kStart) - 1; // excludes the NUL
constexpr std::size_t kEndLen = sizeof(kEnd) - 1;

} // namespace

void BracketPasteDecoder::reset() noexcept {
    clear_state();
    unconsumed_.clear();
    pending_.clear();
    pending_at_ = 0;
}

void BracketPasteDecoder::clear_state() noexcept {
    state_ = 0;
    pos_ = 0;
    pasting_ = false;
}

BracketPasteDecoder::Event BracketPasteDecoder::take_pending(char& out) {
    if (!has_pending())
        return Event::None;
    out = pending_[pending_at_++];
    if (pending_at_ == pending_.size()) {
        pending_.clear();
        pending_at_ = 0;
    }
    return Event::Text;
}

// Inside a paste: only the end marker is special, everything else is content.
BracketPasteDecoder::Event BracketPasteDecoder::feed_inside(int ch, char& out) {
    if (state_ == 3) {
        if (static_cast<char>(ch) == kEnd[pos_]) {
            if (++pos_ == kEndLen) {
                reset();
                return Event::End;
            }
            return Event::None;
        }
        // Not the end marker after all: the bytes held back while it looked like
        // one are content too. Queue them, or pasting text that merely resembles
        // "ESC [ 2 0 1" silently loses it.
        //
        // `ch` goes on the queue as well, after them: emitting it now would put
        // it ahead of the rescued prefix, reversing the pasted text. The caller
        // drains the queue before its next input byte (see has_pending()), so
        // order is preserved end to end.
        for (std::size_t i = 0; i < pos_; ++i)
            pending_.push_back(kEnd[i]);
        pending_.push_back(static_cast<char>(ch));
        pos_ = 0;
        state_ = 2;
        return Event::None;
    }
    if (static_cast<char>(ch) == kEnd[0]) {
        state_ = 3;
        pos_ = 1;
        return Event::None;
    }
    out = static_cast<char>(ch);
    return Event::Text;
}

// Still matching the start marker. A byte that disagrees means this was an
// ordinary ESC, and the bytes read so far go back to the caller so a bare Esc
// still cancels the dialog.
BracketPasteDecoder::Event BracketPasteDecoder::feed_start(int ch) {
    if (static_cast<char>(ch) == kStart[pos_]) {
        if (++pos_ == kStartLen) {
            pasting_ = true;
            state_ = 2;
            pos_ = 0;
            return Event::Begin;
        }
        return Event::None;
    }
    const std::size_t matched = pos_;
    clear_state();
    unconsumed_.push_back(27);
    for (std::size_t i = 1; i < matched; ++i)
        unconsumed_.push_back(static_cast<unsigned char>(kStart[i]));
    return Event::NotPaste;
}

BracketPasteDecoder::Event BracketPasteDecoder::feed(int ch, char& out) {
    if (pasting_)
        return feed_inside(ch, out);
    if (state_ == 0) {
        if (ch != 27)
            return Event::NotPaste;
        state_ = 1;
        pos_ = 1;
        return Event::None;
    }
    return feed_start(ch);
}

PasteBurst consume_paste_burst(BracketPasteDecoder& d, const std::vector<int>& bytes) {
    PasteBurst out;
    char text = 0;
    for (std::size_t i = 0; i < bytes.size();) {
        // A rejected marker may have queued several bytes, and feed() returns at
        // most one per call, so drain them before consuming the next input byte.
        if (d.has_pending()) {
            d.take_pending(text);
            out.insert.push_back(text);
            continue;
        }
        const int b = bytes[i++];
        switch (d.feed(b, text)) {
        case BracketPasteDecoder::Event::Begin:
            out.started = true;
            break;
        case BracketPasteDecoder::Event::Text:
            out.insert.push_back(static_cast<char>(b));
            break;
        case BracketPasteDecoder::Event::NotPaste:
        case BracketPasteDecoder::Event::End:
        case BracketPasteDecoder::Event::None:
            break;
        }
    }
    return out;
}

} // namespace tui