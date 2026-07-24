// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_APP_H
#define AMBER_TUI_APP_H

#include "tui/event_bus.h"
#include "tui/layout.h"

#include <memory>

namespace tui {

class Renderer;
class InputHandler;
class AgentService;
class ChatView;
class StatusBar;
class SessionManager;
class DialogManager;
class CommandDispatcher;
class CompressWorker;

// Thin orchestrator. Runs the tick loop. All components communicate via
// EventBus — App only calls tick() and never knows about their internals.
class App {
public:
    App(std::shared_ptr<EventBus> bus,
        std::unique_ptr<Renderer> renderer,
        std::unique_ptr<InputHandler> input,
        std::unique_ptr<Layout> layout);

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Run the main event loop until QuitRequested.
    void run();

private:
    void tick();

    std::shared_ptr<EventBus> bus_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<InputHandler> input_;
    std::unique_ptr<Layout> layout_;
};

} // namespace tui

#endif // AMBER_TUI_APP_H
