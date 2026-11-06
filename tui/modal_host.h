#ifndef AMBER_TUI_MODAL_HOST_H
#define AMBER_TUI_MODAL_HOST_H

// One modal host for every blocking dialog. It owns the terminal overlay, the
// key loop and the modal flag; the state machines and the painter are pure.
// Because the host never touches the active window, the tab that opened a modal
// is still the active tab when it closes.

#include "agent/agent.h"
#include "tui/dialog_state.h"
#include "tui/dialog_view.h"
#include "tui/widgets.h"

#include <functional>
#include <string>
#include <vector>

namespace tui {

class Terminal;

class ModalHost {
public:
    bool confirm(const std::string& title, const std::string& message);

    // Returns the selected index, or -1 when cancelled.
    int choose(const std::string& title, const std::vector<std::string>& items);

    void info(const std::string& title, const std::vector<std::string>& rows);

    // Edits `fields` in place on OK; returns false on cancel.
    bool form(const std::string& title, std::vector<FieldSpec>& fields,
              const std::string& notice = {});

    agent::Approval approve(const std::string& summary, int timeout_sec, int default_idx);

private:
    enum class Outcome { Continue, Accept, Cancel };
    using Handler = std::function<Outcome(int)>;
    using Tick = std::function<bool()>; // true closes the dialog (timeout)

    Outcome run(DialogView& view, const Handler& handle, int poll_ms, const Tick& tick = {});
    bool handle_paste(FormState& state);
};

} // namespace tui

#endif // AMBER_TUI_MODAL_HOST_H
