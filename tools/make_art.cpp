// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Draws the package's artwork with the app's own renderer:
//   make art   ->  assets/icon0.png (512x512) and assets/pic1.png (1920x1080)
#include <stdio.h>

#include <string>
#include <vector>

#include "gfx/canvas.h"
#include "gfx/font.h"
#include "miniz.h"
#include "ui/widgets.h"

using gfx::Canvas;
using gfx::Face;
using gfx::rgb;

static bool save_png(const std::vector<uint32_t>& px, int w, int h, const std::string& path) {
    std::vector<uint8_t> rgb_bytes((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        rgb_bytes[i * 3] = (px[i] >> 16) & 0xFF;
        rgb_bytes[i * 3 + 1] = (px[i] >> 8) & 0xFF;
        rgb_bytes[i * 3 + 2] = px[i] & 0xFF;
    }
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb_bytes.data(), w, h, 3, &size, 6, MZ_FALSE);
    if (!png) return false;
    FILE* f = fopen(path.c_str(), "wb");
    const bool ok = f && fwrite(png, 1, size, f) == size;
    if (f) fclose(f);
    mz_free(png);
    return ok;
}

// The mark: a download arrow into a tray, white on the tile.
static void arrow(Canvas& c, float x, float y, float size, gfx::Color col) {
    const float cx = x + size / 2, th = size / 10;
    c.line(cx, y + size * 0.12f, cx, y + size * 0.62f, th, col);
    c.line(cx - size * 0.22f, y + size * 0.42f, cx, y + size * 0.64f, th, col);
    c.line(cx + size * 0.22f, y + size * 0.42f, cx, y + size * 0.64f, th, col);
    c.line(x + size * 0.18f, y + size * 0.84f, x + size * 0.82f, y + size * 0.84f, th, col);
}

// A case whose cover is an abstract gradient (no game's artwork): two
// colours and a few soft circles, like key art out of focus.
static void case_art(Canvas& c, const gfx::Rect& r, ui::BoxStyle style, gfx::Color top, gfx::Color bottom,
                     gfx::Color glow) {
    ui::draw_game_box(c, r, "", " ", style);
    const int f = std::max(3, r.w * 3 / 100), band = std::max(14, r.w * 135 / 1000);
    const gfx::Rect window{r.x + f, r.y + f + band, r.w - 2 * f, r.h - 2 * f - band};
    c.push_clip(window);
    c.fill_vgradient(window, top, bottom);
    for (int i = 0; i < 4; ++i)
        c.fill_circle(window.x + window.w * (20 + i * 23) / 100, window.y + window.h * (30 + (i % 2) * 35) / 100,
                      window.w * (40 - i * 6) / 100, gfx::with_alpha(glow, 46));
    c.pop_clip();
}

static void icon(const std::string& path) {
    const int n = 512;
    std::vector<uint32_t> px((size_t)n * n);
    Canvas c(px.data(), n, n, n, n);
    c.fill_vgradient({0, 0, n, n}, rgb(0x1C4FC4), rgb(0x061235));
    // A stack of cases from four generations (PS1, PS2, PS3, PS4), the front
    // one largest. Each sits low enough that the band (and its label) above it shows.
    const int bw = 220, bh = ui::box_height(bw);
    case_art(c, {246, 4, bw - 60, ui::box_height(bw - 60)}, ui::BoxStyle::PS1, rgb(0xE07B2E), rgb(0x4A1A12),
             rgb(0xFFE08A));
    case_art(c, {196, 34, bw - 40, ui::box_height(bw - 40)}, ui::BoxStyle::PS2, rgb(0x0F7A6C), rgb(0x0B2E4F),
             rgb(0x5FE3C0));
    case_art(c, {146, 66, bw - 20, ui::box_height(bw - 20)}, ui::BoxStyle::PS3, rgb(0xC2477A), rgb(0x3A1450),
             rgb(0xFFB36B));
    const gfx::Rect front{96, 100, bw, bh};
    case_art(c, front, ui::BoxStyle::PS4, rgb(0x2F7BFF), rgb(0x1A1450), rgb(0x8FD0FF));
    // The download badge on the front case's corner.
    const int cx = front.right() - 4, cy = front.bottom() - 66, r = 66;
    c.fill_circle(cx, cy, r + 10, rgb(0x061235));
    c.fill_circle(cx, cy, r, rgb(0xFFFFFF));
    arrow(c, cx - 50, cy - 52, 100, rgb(0x2F7BFF));
    // The name.
    c.fill_vgradient({0, 380, n, n - 380}, gfx::with_alpha(rgb(0x061235), 0), rgb(0x061235));
    const int fw = gfx::text_width(Face::SansBold, 84, "FPKGi+");
    const int pw = gfx::text_width(Face::SansBold, 84, "+");
    gfx::draw_text(c, Face::SansBold, 84, (n - fw) / 2, 392, "FPKGi", rgb(0xFFFFFF));
    gfx::draw_text(c, Face::SansBold, 84, (n + fw) / 2 - pw, 392, "+", rgb(0x5FA8FF));
    printf(save_png(px, n, n, path) ? "wrote %s\n" : "FAILED %s\n", path.c_str());
}

static void picture(const std::string& path) {
    const int w = 1920, h = 1080;
    std::vector<uint32_t> px((size_t)w * h);
    Canvas c(px.data(), w, h, w, h);
    c.fill_vgradient({0, 0, w, h}, rgb(0x121A2E), rgb(0x0A0F1C));
    // A shelf of cases from every supported platform, fading out.
    const int bw = 230, bh = ui::box_height(bw);
    // One case per platform, in release order.
    static const ui::BoxStyle styles[] = {ui::BoxStyle::PS1, ui::BoxStyle::PS2, ui::BoxStyle::PSP, ui::BoxStyle::PS3,
                                          ui::BoxStyle::PS4};
    constexpr int kCases = 5;
    const int step = (w - 2 * 120 - bw) / (kCases - 1);
    static const gfx::Color covers[][3] = {
        {rgb(0x2F7BFF), rgb(0x1A1450), rgb(0x8FD0FF)}, {rgb(0xC2477A), rgb(0x3A1450), rgb(0xFFB36B)},
        {rgb(0x0F7A6C), rgb(0x0B2E4F), rgb(0x5FE3C0)}, {rgb(0xE07B2E), rgb(0x4A1A12), rgb(0xFFE08A)},
        {rgb(0x6A4BD8), rgb(0x160F3A), rgb(0xC9A8FF)}, {rgb(0x2A9D4B), rgb(0x0C2A1A), rgb(0xB8F5A0)},
        {rgb(0xD23B3B), rgb(0x2A0B16), rgb(0xFF9E9E)}};
    for (int i = 0; i < kCases; ++i) {
        const gfx::Rect r{120 + i * step, 600, bw, bh};
        case_art(c, r, styles[i], covers[i][0], covers[i][1], covers[i][2]);
    }
    c.fill_vgradient({0, 560, w, h - 560}, gfx::with_alpha(rgb(0x0A0F1C), 90), rgb(0x0A0F1C));
    c.fill_rounded({120, 250, 150, 150}, 34, rgb(0x2F7BFF));
    arrow(c, 138, 266, 114, rgb(0xFFFFFF));
    const int tw = gfx::draw_text(c, Face::SansBold, 130, 310, 236, "FPKGi", rgb(0xE9EEF7));
    gfx::draw_text(c, Face::SansBold, 130, 310 + tw + 4, 236, "+", rgb(0x2F7BFF));
    gfx::draw_text(c, Face::Sans, 40, 316, 420, "Your FPKGi content, as a store.", rgb(0x9AA6BD));
    printf(save_png(px, w, h, path) ? "wrote %s\n" : "FAILED %s\n", path.c_str());
}

int main() {
    gfx::set_render_scale(1.f);
    if (!gfx::fonts_init("third_party/fonts")) fprintf(stderr, "fonts missing\n");
    icon("assets/icon0.png");
    picture("assets/pic1.png");
    return 0;
}
