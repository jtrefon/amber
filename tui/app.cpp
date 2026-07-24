// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/app.h"
#include "tui/renderer.h"
#include "tui/input_handler.h"

#include <chrono>
#include <thread>

namespace tui {

App::App(std::shared_ptr<EventBus> bus,
         std::unique_ptr<Renderer> renderer,
         std::unique_ptr<InputHandler> input,
         std::unique_ptr<Layout> layout)
    : bus_(std::move(bus))
    , renderer_(std::move(renderer))
    , input_(std::move(input))
    , layout_(std::move(layout)) {}

void App::run() {
    bool quitting = false;
    bus_->on<QuitRequested>([&](const QuitRequested&) { quitting = true; });

    while (!quitting) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void App::tick() {
    bus_->dispatch();
    input_->poll();
    renderer_->draw(*layout_, input_->input(), input_->drawer_open());
}

} // namespace tui
