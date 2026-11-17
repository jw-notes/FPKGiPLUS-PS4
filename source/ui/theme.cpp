// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/theme.h"

#include "gfx/font.h"
#include "platform/platform.h"

namespace ui {

using gfx::rgb;

namespace {
ThemeId g_theme = ThemeId::Modern;
}
void set_theme(ThemeId id) {
    g_theme = id;
    gfx::set_pixel_text(id == ThemeId::Arcade);
    gfx::set_square_corners(id == ThemeId::Arcade);
}
bool blink_on() { return (platform::now_ms() / 350) % 2 == 0; }
ThemeId theme_id() { return g_theme; }

const Theme& theme() {
    // A cabinet in a dark arcade: near-black purple, hot pink, cyan and a
    // yellow cursor.
    static const Theme neon{rgb(0x000000), rgb(0x0A0420), rgb(0x0B0624), rgb(0x24105C), rgb(0xFFFFFF), rgb(0x00E5FF),
                            rgb(0x8A7BD0), rgb(0xFF2D95), rgb(0xFFFFFF), rgb(0x00E5FF), rgb(0xFFE600), rgb(0xFF4040),
                            rgb(0x39FF14), rgb(0xFF9A00)};
    if (g_theme == ThemeId::Arcade) return neon;
    // A deep navy store, tuned for a television in a dim room: no pure white,
    // no pure black. The accent is the PlayStation blue of the box art.
    static const Theme dark{rgb(0x0A0F1C), rgb(0x121A2E), rgb(0x18223A), rgb(0x24314F), rgb(0xE9EEF7), rgb(0x9AA6BD),
                            rgb(0x5B6680), rgb(0x2F7BFF), rgb(0xFFFFFF), rgb(0x26314A), rgb(0x6FA8FF), rgb(0xFF6B6B),
                            rgb(0x46C98B), rgb(0xF2C14E)};
    return dark;
}

}  // namespace ui
