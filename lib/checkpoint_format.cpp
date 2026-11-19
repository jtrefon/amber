#include "agent/checkpoint_format.h"

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

namespace agent {

namespace {

std::string clip(const std::string& text, std::size_t max) {
    if (text.size() <= max)
        return text;
    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    return text.substr(0, end) + "...";
}

std::string flatten(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (unsigned char c : text) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = true;
            continue;
        }
        if (space && !out.empty())
            out.push_back(' ');
        space = false;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string turn_label(const Checkpoint& checkpoint) {
    if (checkpoint.pre())
        return "before turn " + std::to_string(checkpoint.turn + 1);
    return "turn " + std::to_string(checkpoint.turn);
}

} // namespace

std::string checkpoint_short_id(const Checkpoint& checkpoint) {
    return checkpoint.id.size() > 8 ? checkpoint.id.substr(0, 8) : checkpoint.id;
}

std::string checkpoint_describe(const Checkpoint& checkpoint) {
    return turn_label(checkpoint) + " (" + checkpoint_short_id(checkpoint) + ")";
}

std::string checkpoint_when(long long timestamp_s) {
    if (timestamp_s <= 0)
        return "unknown";
    const auto value = static_cast<std::time_t>(timestamp_s);
    std::tm local{};
    if (localtime_r(&value, &local) == nullptr)
        return "unknown";
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local) == 0)
        return "unknown";
    return buffer;
}

std::vector<std::string> checkpoint_rows(const std::vector<Checkpoint>& checkpoints) {
    std::vector<std::string> rows;
    rows.reserve(checkpoints.size());
    for (const auto& checkpoint : checkpoints) {
        std::string row = turn_label(checkpoint);
        row += "  " + checkpoint_short_id(checkpoint);
        row += "  " + std::to_string(checkpoint.files) + " files";
        row += "  " + checkpoint_when(checkpoint.timestamp_s);
        row += "  " + clip(flatten(checkpoint.prompt), 80);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string checkpoint_history_text(const std::vector<Checkpoint>& checkpoints) {
    std::string out;
    for (const auto& row : checkpoint_rows(checkpoints)) {
        if (!out.empty())
            out += "\n";
        out += row;
    }
    return out;
}

} // namespace agent
