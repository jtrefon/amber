#ifndef AGENT_UI_SERVICES_H
#define AGENT_UI_SERVICES_H

// Talking to the user (spec §8).
//
// A plugin that needs something only the user can supply — a key, a choice, a
// confirmation — asks through this port. The host owns the modal, the terminal,
// and the threading; the plugin gets an answer or a defined "no".
//
// Kept out of `HostServices`: that struct is the *harness* services a tool binds
// to (jobs, todos, subagents, the cancel token), which are inert. These are
// user-mediated, they can block, and they can fail closed. Two different things
// deserve two different names, and the guide already calls this one `ui()`.

#include <functional>
#include <string>
#include <vector>

namespace agent {

// Where a notification sits in the user's attention, not a log severity.
enum class UiLevel { Info, Warn, Error };

// The words a host shows; also the prefix a notification gets when it is not
// merely informational.
inline const char* to_string(UiLevel level) noexcept {
    switch (level) {
    case UiLevel::Warn:
        return "warning";
    case UiLevel::Error:
        return "error";
    case UiLevel::Info:
        break;
    }
    return "notice";
}

struct AskSpec {
    std::string title;   // the dialog's title ("API key required")
    std::string prompt;  // the question, in one line
    std::string initial; // pre-filled value, usually empty
};

struct ChooseSpec {
    std::string title;
    std::vector<std::string> choices;
    int default_index = 0;
};

struct ConfirmSpec {
    std::string title;
    std::string message;
};

class UiServices {
public:
    virtual ~UiServices() = default;

    // Blocking: called on the plugin's thread, answered by the host's UI
    // thread. Empty string means the user cancelled — the same contract as
    // every cancelled input, so a caller cannot mistake it for a value.
    virtual std::string ask_text(const AskSpec& spec) = 0;
    virtual std::string ask_secret(const AskSpec& spec) = 0;

    // Index into `choices`, or -1 when cancelled.
    virtual int choose(const ChooseSpec& spec) = 0;

    // False unless the user affirmatively agreed. A host with no way to ask
    // returns false rather than assuming yes.
    virtual bool confirm(const ConfirmSpec& spec) = 0;

    // Non-blocking, callable from any thread: the host queues and shows it.
    virtual void notify(UiLevel level, const std::string& message) = 0;

    // The sanctioned way to touch UI state from another thread: the callable
    // runs later on the host's UI thread, where render callables are safe.
    virtual void post_to_ui(std::function<void()> work) = 0;
};

// What a host with no way to ask attaches, and what a plugin gets when the host
// attached nothing. Every question fails closed and says so, so a plugin never
// has to null-check its way to a safe default: a missing UI degrades to "no",
// never to a guessed yes.
class NullUiServices : public UiServices {
public:
    std::string ask_text(const AskSpec&) override { return {}; }
    std::string ask_secret(const AskSpec&) override { return {}; }
    int choose(const ChooseSpec&) override { return -1; }
    bool confirm(const ConfirmSpec&) override { return false; }
    void notify(UiLevel, const std::string&) override {}
    void post_to_ui(std::function<void()>) override {}
};

// Process-wide null instance, so wiring code can point at one and callers never
// see a dangling pointer.
inline UiServices& null_ui_services() noexcept {
    static NullUiServices instance;
    return instance;
}

} // namespace agent

#endif // AGENT_UI_SERVICES_H
