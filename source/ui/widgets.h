// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Drawing helpers shared by the screens: the header with its tabs, the footer
// of button prompts, game boxes, chips, spinners, messages, and the list/grid
// focus logic.
#include <string>
#include <vector>

#include "gfx/canvas.h"
#include "gfx/font.h"
#include "ui/screen.h"
#include "ui/theme.h"

namespace ui {

// Layout grid for 1920x1080, inside the TV-safe area.
constexpr int kMarginX = 96;
constexpr int kHeaderH = 132;
constexpr int kContentTop = 150;
constexpr int kFooterTop = 996;
constexpr int kContentW = 1920 - 2 * kMarginX;

constexpr int kTitlePx = 46;
constexpr int kHeadPx = 34;
constexpr int kBodyPx = 28;
constexpr int kSmallPx = 23;

// A game box is as tall as a real PS4 case: 1.255 x its width.
inline int box_height(int width) { return width * 1255 / 1000; }

void draw_background(gfx::Canvas& c);
// `active`: the highlighted tab (-1: none).
void draw_header(gfx::Canvas& c, int active);
void draw_footer(gfx::Canvas& c, const std::vector<Hint>& hints);
// Draws a DualShock button glyph centred vertically on `cy`; returns its width.
int draw_button_glyph(gfx::Canvas& c, uint32_t button, int x, int cy, int size);
void draw_spinner(gfx::Canvas& c, int cx, int cy, int radius);
void draw_loading(gfx::Canvas& c, const gfx::Rect& area, const std::string& label);
void draw_message(gfx::Canvas& c, const gfx::Rect& area, const std::string& title, const std::string& body,
                  bool error = false);
void draw_progress(gfx::Canvas& c, const gfx::Rect& r, float value, gfx::Color color = 0);
void draw_scrollbar(gfx::Canvas& c, const gfx::Rect& track, int first, int visible, int total);
// A small rounded label ("USA", "v1.04"); returns its width.
int draw_chip(gfx::Canvas& c, int x, int y, const std::string& text, gfx::Color fg, gfx::Color bg, int px = 22);
int chip_width(const std::string& text, int px = 22);
std::string clock_text();

// The case a title is drawn in.
enum class BoxStyle { PS4, PS5, PS1, PS2, PS3, PSP };
const char* box_label(BoxStyle style);  // "PS4", "PS2", ...

// A game in its case: PS4 (blue, with the PS4 band), PS5 (white band), PS1
// (grey), PS2 (black), PS3 (smoked, silver band) or PSP (clear, black band), the cover art filling the
// window below the band. `art` is an image-cache source; until it loads, a
// generated cover shows the title.
void draw_game_box(gfx::Canvas& c, const gfx::Rect& r, const std::string& art, const std::string& title, BoxStyle style,
                   bool shadow = true);

// Focus for a vertical list: index, and the first visible row.
struct ListNav {
    int index = 0;
    int top = 0;
    // Handles Up/Down (and Left/Right as page jumps when `paging`); true if moved.
    bool update(const Input& in, int count, int visible, bool paging = true);
    void clamp(int count, int visible);
};

// Focus for a grid of `cols` columns.
struct GridNav {
    int index = 0;
    int top_row = 0;
    bool update(const Input& in, int count, int cols, int visible_rows);
    void clamp(int count, int cols, int visible_rows);
};

}  // namespace ui
