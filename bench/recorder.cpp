
// RED: stub — recorder not implemented yet (empty stream).

#include "bench/recorder.h"

namespace bench {

agent::AgentHooks Recorder::hooks() const { return {}; }

void Recorder::on_tool_call(const std::string&, const agent::json&) {}
void Recorder::on_tool_result(const std::string&, const agent::ToolResult&) {}
void Recorder::on_status(const std::string&) {}
void Recorder::on_debug(const std::string&) {}
void Recorder::on_stats(const agent::Stats&) {}
void Recorder::on_state(agent::RunState) {}

std::string Recorder::fingerprint(const std::string&, const agent::json&) noexcept {
    return "";
}

long Recorder::now_ms() noexcept { return 0; }

void parse_status(const std::string&, EventStream&) noexcept {}
void parse_debug(const std::string&, EventStream&) noexcept {}

} // namespace bench
