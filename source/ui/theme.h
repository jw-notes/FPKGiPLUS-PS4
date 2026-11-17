// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include "gfx/canvas.h"

namespace ui {

struct Theme {
    gfx::Color bg, bg_top, surface, surface_hi, text, muted, faint, accent, accent_text, divider, focus, error, success,
        warning;
};

// Modern: the navy store with a grid of covers. Arcade: a neon cabinet menu,
// where the store is a list to pick from and scanlines cover the screen.
enum class ThemeId { Modern, Arcade };
void set_theme(ThemeId id);
ThemeId theme_id();
inline bool arcade() { return theme_id() == ThemeId::Arcade; }
// The arcade cursor's blink: on and off about twice a second.
bool blink_on();

const Theme& theme();

}  // namespace ui
