#include "tui/completion_context.h"

#include "tui/drawer_rows.h"

namespace tui::completion_context {

Context for_input(const std::string& input, const SettingRegistry& settings) {
    Context ctx;
    if (input.empty() || input[0] != '/') {
        // Non-slash text: top-level command names from the tree, including
        // JSON-declared aliases — never a hardcoded list.
        ctx.rows = settings.complete("");
        for (const auto& a : settings.top_level_aliases())
            ctx.rows.push_back(a);
        return ctx;
    }

    // The drawer is the visible contract: feed exactly its entry names so arrow
    // selection and Enter dispatch index the same rows the user sees (aliases
    // are not drawer rows).
    ctx.rows = drawer_entry_names(input, settings);

    // Dispatch prefix Enter prepends to the selected row:
    //   "/window"     (namespace descend) -> "/window " (keep the namespace)
    //   "/c"          (partial)           -> "/"        (replace the token)
    //   "/set model " (trailing space)    -> "/set model "
    if (input.back() == ' ') {
        ctx.prefix = input; // explicit descend
        return ctx;
    }
    size_t tok_start = input.rfind(' ');
    tok_start = (tok_start == std::string::npos) ? 1 : tok_start + 1;
    const std::string last_tok = input.substr(tok_start);

    // drawer_entry_names descends into a namespace when the trailing token
    // resolves to children (drawer_rows.cpp); then the namespace is KEPT.
    // Otherwise the token is a partial being typed and is REPLACED.
    std::string ns_so_far = input.substr(1, tok_start - 1);
    while (!ns_so_far.empty() && ns_so_far.back() == ' ')
        ns_so_far.pop_back();
    std::string probe = ns_so_far;
    if (!probe.empty())
        probe += '.';
    probe += last_tok;

    if (settings.children_of(probe).empty()) {
        ctx.prefix = input.substr(0, tok_start);
    } else {
        ctx.prefix = input;
        ctx.prefix += ' ';
    }
    return ctx;
}

} // namespace tui::completion_context
