
#include "agent/llm.h"
#include "agent/debug_log.h"
#include "agent/dialect.h"
#include "http_transport.h"
#include "agent/model_probe.h"
#include "agent/context.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <utility>

namespace agent {

namespace {

// The transport teaches the runtime context window by mutating the Config it
// was handed on a 400 overflow rejection — and then throws. Capturing the
// change as the scope exits covers BOTH paths; capturing it after the call
// alone was unreachable, because the only code that mutates the window always
// throws. Without this the host's clamp (Agent::resolve_window) never fires.
class LearnedWindowCapture {
public:
    LearnedWindowCapture(const Config& cfg, int& learned, std::function<void(int)> on_learned = {})
        : cfg_(cfg), before_(cfg.context_size), learned_(learned),
          on_learned_(std::move(on_learned)) {}
    ~LearnedWindowCapture() {
        if (cfg_.context_size == before_)
            return;
        learned_ = cfg_.context_size;
        if (on_learned_)
            on_learned_(learned_);
    }
    LearnedWindowCapture(const LearnedWindowCapture&) = delete;
    LearnedWindowCapture& operator=(const LearnedWindowCapture&) = delete;
    LearnedWindowCapture(LearnedWindowCapture&&) = delete;
    LearnedWindowCapture& operator=(LearnedWindowCapture&&) = delete;

private:
    const Config& cfg_;
    const int before_;
    int& learned_;
    std::function<void(int)> on_learned_;
};

// The completion budget is counted against the SAME window as the prompt: a
// max_tokens carried over from a bigger endpoint pushes an otherwise-fitting
// request over a smaller provider's limit (llama.cpp's own overflow 400 says
// so). Fit the ask to the window the request is actually going to; with no
// known window the configured ask stands.
Config fit_completion_budget(const Config& cfg, const std::vector<Message>& messages) {
    if (cfg.context_size <= 0)
        return cfg;
    const long room =
        static_cast<long>(cfg.context_size) - static_cast<long>(estimate_tokens(messages));
    if (room >= static_cast<long>(cfg.max_tokens))
        return cfg;
    Config fitted = cfg;
    fitted.max_tokens = static_cast<size_t>(std::max(1L, room));
    return fitted;
}

// Resolve the wire protocol: an explicit dialect wins, otherwise the configured
// flavor picks one. A flavor whose provider plugin is switched off refuses here,
// loudly — the silent openai fallback is right for a typo in a provider file and
// wrong for this, where the endpoint and key are configured and the request
// would quietly speak the wrong protocol.
//
// This runs before the member is initialised, precisely so the check does not
// have to look at the constructor's parameter after it has been moved from.
std::unique_ptr<Dialect> resolve_dialect(const Config& cfg,
                                         std::unique_ptr<Dialect> explicit_dialect) {
    if (explicit_dialect)
        return explicit_dialect;
    const std::string reason = flavor_unavailable_reason(cfg.flavor);
    if (!reason.empty())
        throw std::runtime_error(reason);
    return make_dialect(cfg.flavor);
}

} // namespace

HttpLLMClient::HttpLLMClient(Config cfg) : HttpLLMClient(std::move(cfg), nullptr) {}

HttpLLMClient::HttpLLMClient(Config cfg, std::unique_ptr<Dialect> dialect)
    : cfg_(std::move(cfg)), dialect_(resolve_dialect(cfg_, std::move(dialect))) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

HttpLLMClient::~HttpLLMClient() = default;

void HttpLLMClient::add_notice(std::string text) {
    std::scoped_lock lk(notices_mtx_);
    notices_.push_back(std::move(text));
}

std::vector<std::string> HttpLLMClient::take_notices() {
    std::scoped_lock lk(notices_mtx_);
    return std::exchange(notices_, {});
}

ServerInfo HttpLLMClient::probe_server() const {
    return agent::probe_server(cfg_, *dialect_);
}

Message HttpLLMClient::chat(const std::vector<Message>& messages,
                            const std::vector<std::shared_ptr<Tool>>& tools, Stats* stats) {
    json body =
        dialect_->build_chat_body(fit_completion_budget(cfg_, messages), messages, tools, false);
    // Tool/model text can contain invalid UTF-8 (e.g. binary from grep);
    // nlohmann throws type_error.316 on dump() unless we replace bad bytes.
    std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);
    debug_log(cfg_.debug_dump_path(), "request", payload);

    double ttfb = 0, total = 0;
    LearnedWindowCapture learned_window(cfg_, learned_, [this](int window) {
        add_notice("the server rejected the prompt size; the context window is now " +
                   std::to_string(window) + " tokens");
    });
    std::string response = post_completion(cfg_, *dialect_, payload, false, &ttfb, &total);
    debug_log(cfg_.debug_dump_path(), "response", response);

    Message out = dialect_->parse_completion(response);
    if (stats)
        fill_buffered_stats(*stats, *dialect_, response, ttfb, total);
    return out;
}

// Drop tool calls whose arguments are not JSON. Malformed calls in history
// poison every later request (the server rejects them), so the whole batch goes
// and the text stands — and the drop is reported, because a call the model
// believes it made silently vanishing is how it re-issues the same work.
bool drop_malformed_tool_calls(Message& out) {
    if (out.tool_calls.is_null() || !out.tool_calls.is_array())
        return false;
    for (const auto& tc : out.tool_calls) {
        auto fn = tc.value("function", json::object());
        std::string raw = fn.value("arguments", "");
        if (raw.empty())
            continue;
        auto parsed = json::parse(raw, nullptr, false);
        if (parsed.is_discarded()) {
            out.tool_calls = json::value_t::null;
            return true;
        }
    }
    return false;
}

Message HttpLLMClient::chat_stream(const std::vector<Message>& messages,
                                   const std::vector<std::shared_ptr<Tool>>& tools,
                                   const std::function<void(const StreamChunk&)>& on_chunk,
                                   Stats* stats) {
    json body =
        dialect_->build_chat_body(fit_completion_budget(cfg_, messages), messages, tools, true);
    std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);
    debug_log(cfg_.debug_dump_path(), "request-stream", payload);

    Message out;
    out.role = "assistant";
    auto decoder = dialect_->make_decoder(out, on_chunk, cfg_.debug_dump_path());

    long status = 0;
    LearnedWindowCapture learned_window(cfg_, learned_, [this](int window) {
        add_notice("the server rejected the prompt size; the context window is now " +
                   std::to_string(window) + " tokens");
    });
    stream_completion(cfg_, *dialect_, payload, *decoder, stats, status);
    debug_log(cfg_.debug_dump_path(), "response-stream",
              "http=" + std::to_string(status) + " content=" + out.content + "\n---reasoning---\n" +
                  out.reasoning);

    if (drop_malformed_tool_calls(out)) {
        debug_log(cfg_.debug_dump_path(), "response-stream", "discarding malformed tool calls");
        add_notice("a tool call arrived with invalid arguments and was dropped for this request; "
                   "the reply's text is what stands");
    }

    return out;
}

} // namespace agent
