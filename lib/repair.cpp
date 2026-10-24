#include "agent/repair.h"

namespace agent {

namespace {

// The tool grammar for the loaded template could not be built. Dropping the
// tools costs the model its hands for one attempt, so it is only a repair when
// the request had tools to drop.
class TemplateParserRepair final : public RepairStrategy {
public:
    RequestFailure handles() const noexcept override { return RequestFailure::TemplateParser; }

    RetryRequest apply(const std::string& error_text, const std::string& stage, bool tools_present,
                       RepairHost& host) const override {
        (void)stage;
        host.report_error("template_parser", error_text);
        if (!tools_present)
            return RetryRequest::None;
        host.announce("[harness] the server could not parse the tool grammar for this model; the "
                      "request is being retried without tools.");
        return RetryRequest::WithoutTools;
    }
};

// The server does not serve the configured model. The first model it advertises
// is the one it is willing to answer for, and that is a one-line switch.
class ServerModelRepair final : public RepairStrategy {
public:
    RequestFailure handles() const noexcept override { return RequestFailure::ModelName; }

    RetryRequest apply(const std::string& error_text, const std::string& stage, bool tools_present,
                       RepairHost& host) const override {
        (void)stage;
        (void)tools_present;
        host.report_error("model_name", error_text);
        const std::vector<ModelInfo> models = host.server_models();
        if (models.empty() || models.front().id == host.model())
            return RetryRequest::None;
        const ModelInfo& replacement = models.front();
        host.adopt_model(replacement.id, capabilities_of(replacement));
        host.status("server rejected model - retrying with \"" + replacement.id + "\"");
        host.announce("[harness] the server rejected the configured model; this request is being "
                      "retried with \"" +
                      replacement.id + "\".");
        return RetryRequest::WithTools;
    }
};

// The key was rejected. The host may have another one (a keyring, a prompt);
// with no key to swap in, the turn degrades exactly as before.
class ApiKeyRepair final : public RepairStrategy {
public:
    RequestFailure handles() const noexcept override { return RequestFailure::Auth; }

    RetryRequest apply(const std::string& error_text, const std::string& stage, bool tools_present,
                       RepairHost& host) const override {
        (void)tools_present;
        host.report_error("auth", error_text);
        const std::string key = host.request_api_key(stage.c_str());
        if (key.empty())
            return RetryRequest::None;
        host.adopt_api_key(key);
        host.announce("[harness] the API key was rejected and has been replaced; this request is "
                      "being retried with the new key.");
        return RetryRequest::WithTools;
    }
};

} // namespace

void RepairLadder::add(std::unique_ptr<RepairStrategy> strategy) {
    if (strategy)
        strategies_.push_back(std::move(strategy));
}

RetryRequest RepairLadder::repair(const std::string& error_text, const std::string& stage,
                                  bool tools_present, RepairHost& host) const {
    const RequestFailure failure = classify_request_failure(error_text);
    if (failure == RequestFailure::None)
        return RetryRequest::None;
    for (const auto& strategy : strategies_) {
        if (strategy->handles() != failure)
            continue;
        const RetryRequest request = strategy->apply(error_text, stage, tools_present, host);
        if (request != RetryRequest::None)
            return request;
    }
    return RetryRequest::None;
}

std::unique_ptr<RepairStrategy> make_template_parser_repair() {
    return std::make_unique<TemplateParserRepair>();
}

std::unique_ptr<RepairStrategy> make_server_model_repair() {
    return std::make_unique<ServerModelRepair>();
}

std::unique_ptr<RepairStrategy> make_api_key_repair() {
    return std::make_unique<ApiKeyRepair>();
}

RepairLadder make_default_repair_ladder() {
    RepairLadder ladder;
    ladder.add(make_template_parser_repair());
    ladder.add(make_server_model_repair());
    ladder.add(make_api_key_repair());
    return ladder;
}

} // namespace agent
