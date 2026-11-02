#ifndef AMBER_TUI_HINTS_H
#define AMBER_TUI_HINTS_H

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// L1 (pure): a shuffled bag of idle hints. The caller decides when the prompt
// is eligible; this class decides which hint, and for how long.
namespace tui {

class HintRotator {
public:
    HintRotator() = default;
    explicit HintRotator(std::vector<std::string> hints, std::uint32_t seed = 1);

    void set_hints(std::vector<std::string> hints);
    // idle = time since the last key. Returns the hint to paint, or "".
    std::string update(std::chrono::milliseconds idle, bool eligible);
    void reset();
    bool empty() const noexcept { return hints_.empty(); }

    static constexpr std::chrono::milliseconds kDelay{5000};
    static constexpr std::chrono::milliseconds kEvery{12000};

private:
    void refill();
    void pick();

    std::vector<std::string> hints_;
    std::vector<std::size_t> bag_;
    std::size_t next_ = 0;
    std::uint32_t seed_ = 1;
    std::string current_;
    std::chrono::milliseconds next_at_{0};
    bool finished_ = false;
};

// The shipped file, or a built-in list when the file is missing. Empty strings
// and malformed JSON are ignored.
std::vector<std::string> load_hints(const std::string& path);
std::vector<std::string> default_hints();

} // namespace tui

#endif // AMBER_TUI_HINTS_H
