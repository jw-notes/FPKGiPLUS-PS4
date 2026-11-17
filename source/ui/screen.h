// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <string>
#include <vector>

#include "core/worker.h"
#include "gfx/canvas.h"
#include "ui/input.h"

namespace ui {

// One button prompt in the footer: "✕ Open".
struct Hint {
    uint32_t button;
    std::string label;
};

class Screen {
public:
    virtual ~Screen() { *token_ = true; }
    virtual void update(const Input& in) = 0;
    virtual void draw(gfx::Canvas& c) = 0;
    // True while something moves on its own (a spinner, a download).
    virtual bool animating() const { return false; }
    // Called when the screen becomes the top one again.
    virtual void resumed() {}
    // Overlays (menus, the keyboard, dialogs) draw over the screen below.
    virtual bool overlay() const { return false; }
    // Draws its own backdrop (the artwork of the focused title) instead of the plain one.
    virtual bool own_background() const { return false; }
    // The header tab this screen belongs to (an index into tab_names()), or -1.
    virtual int tab() const { return -1; }
    virtual std::vector<Hint> hints() const { return {}; }

protected:
    // Cancelled when the screen is destroyed: results of background work for a
    // closed screen are dropped.
    lib::CancelToken token_ = lib::make_token();
};

}  // namespace ui
