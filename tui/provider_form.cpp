#include "tui/provider_form.h"

#include "agent/provider_health.h"
#include "tui/widgets.h"

#include <string>
#include <vector>

namespace tui {

bool edit_provider_form(agent::Config& cfg, const std::string& title) {
    std::string model_field = cfg.model_explicit ? cfg.model : "";
    std::vector<FieldSpec> fields = {
        {"Server URL", cfg.api_base, false},
        {"API Key", cfg.api_key, true},
        {"Model (blank = auto)", model_field, false},
    };
    std::string notice;
    for (;;) {
        if (!form_edit(title, fields, notice))
            return false;
        cfg.api_base = fields[0].value;
        while (!cfg.api_base.empty() && cfg.api_base.back() == '/')
            cfg.api_base.pop_back();
        cfg.api_key = fields[1].value;
        if (fields[2].value.empty()) {
            cfg.model_explicit = false;
        } else {
            cfg.model = fields[2].value;
            cfg.model_explicit = true;
        }
        // An empty key has nothing to verify (a keyless local endpoint is a
        // valid configuration). Anything else is checked before it is saved:
        // a rejected key keeps the form open with the reason, so it can never
        // be written to disk and discovered later as "no answer".
        if (cfg.api_key.empty())
            return true;
        const agent::ConnectionCheck check = agent::validate_connection(cfg);
        if (!check.auth_rejected)
            return true;
        notice = "Rejected: " + check.reason + " - fix the key or press Esc";
    }
}

} // namespace tui
