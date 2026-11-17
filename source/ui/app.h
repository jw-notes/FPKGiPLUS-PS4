// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <memory>
#include <string>
#include <vector>

#include "gfx/canvas.h"
#include "ui/screen.h"

namespace ui {

class App {
public:
    void run();
    void push(std::unique_ptr<Screen> screen);
    // Closes the top screen (at the next frame, so a screen may close itself).
    void pop();
    // Replaces the whole stack with a tab's root screen.
    void switch_tab(int tab);
    void toast(const std::string& message);
    void quit() { quit_ = true; }
    void close_all() {
        pending_push_.clear();
        pending_root_.reset();
        while (!stack_.empty()) stack_.pop_back();
    }
    // The screen below the top one (for overlays that act on it).
    Screen* below(const Screen* s);

private:
    void apply_pending();
    void draw();

    std::vector<std::unique_ptr<Screen>> stack_;
    std::vector<std::unique_ptr<Screen>> pending_push_;
    int pending_pops_ = 0;
    std::unique_ptr<Screen> pending_root_;
    std::string toast_;
    uint64_t toast_until_ = 0;
    bool quit_ = false;
    InputReader input_;
};

App& app();

// The header's tabs: one per category that has titles, then Downloads
// (screens.cpp). Rebuilt when the catalogue is reloaded.
std::vector<std::string> tab_names();
std::unique_ptr<Screen> make_tab_screen(int tab);

}  // namespace ui
