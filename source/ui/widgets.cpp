// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/widgets.h"

#include <math.h>
#include <time.h>

#include <algorithm>

#include "core/worker.h"
#include "gfx/image_cache.h"
#include "platform/platform.h"
#include "store/downloads.h"
#include "ui/app.h"

namespace ui {

using gfx::Canvas;
using gfx::Color;
using gfx::Face;
using gfx::Rect;
using namespace platform;

void draw_background(Canvas& c) {
    const Theme& t = theme();
    c.fill_vgradient({0, 0, c.width(), c.height()}, t.bg_top, t.bg);
    if (arcade()) {
        // A faint grid, like the test pattern of an old cabinet.
        const gfx::Color line = gfx::with_alpha(gfx::rgb(0x6A2CFF), 38);
        for (int x = 0; x < c.width(); x += 64) c.fill({x, 0, 2, c.height()}, line);
        for (int y = 0; y < c.height(); y += 64) c.fill({0, y, c.width(), 2}, line);
    }
}

std::string clock_text() {
    const time_t now = time(nullptr) + utc_offset_seconds();
    tm t{};
    gmtime_r(&now, &t);
    char text[16];
    if (clock_24h()) snprintf(text, sizeof text, "%02d:%02d", t.tm_hour, t.tm_min);
    else snprintf(text, sizeof text, "%d:%02d %s", (t.tm_hour + 11) % 12 + 1, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
    return text;
}

// The mark: a rounded tile with a download arrow into a tray.
static void draw_logo(Canvas& c, int x, int y, int size) {
    const Theme& t = theme();
    c.fill_rounded({x, y, size, size}, size / 4, t.accent);
    const float cx = x + size / 2.f, th = size / 11.f;
    const Color w = gfx::rgb(0xFFFFFF);
    c.line(cx, y + size * 0.2f, cx, y + size * 0.62f, th, w);
    c.line(cx - size * 0.18f, y + size * 0.45f, cx, y + size * 0.63f, th, w);
    c.line(cx + size * 0.18f, y + size * 0.45f, cx, y + size * 0.63f, th, w);
    c.line(x + size * 0.24f, y + size * 0.78f, x + size * 0.76f, y + size * 0.78f, th, w);
}

// A download arrow, for the header's count of running downloads.
static void draw_download_icon(Canvas& c, int x, int cy, int size, Color col) {
    const float cx = x + size / 2.f, th = std::max(2.f, size / 9.f);
    c.line(cx, cy - size * 0.42f, cx, cy + size * 0.12f, th, col);
    c.line(cx - size * 0.24f, cy - size * 0.1f, cx, cy + size * 0.14f, th, col);
    c.line(cx + size * 0.24f, cy - size * 0.1f, cx, cy + size * 0.14f, th, col);
    c.line(x + size * 0.12f, cy + size * 0.4f, x + size * 0.88f, cy + size * 0.4f, th, col);
}

void draw_header(Canvas& c, int active) {
    const Theme& t = theme();
    // A soft band behind the header, so artwork backdrops never fight the tabs.
    c.fill_vgradient({0, 0, c.width(), kHeaderH + 10}, gfx::with_alpha(t.bg, 235), gfx::with_alpha(t.bg, 0));
    draw_logo(c, kMarginX, 44, 50);
    const int name_w = gfx::draw_text(c, Face::SansBold, 40, kMarginX + 66, 42, "FPKGi", t.text);
    gfx::draw_text(c, Face::SansBold, 40, kMarginX + 66 + name_w + 2, 42, "+", t.accent);

    const std::vector<std::string> tabs = tab_names();
    int px = 25, pad = 18;
    const int gap = 6;
    auto measure = [&] {
        int total = 0;
        for (const auto& name : tabs) total += gfx::text_width(Face::SansSemiBold, px, name) + 2 * pad + gap;
        return total - gap;
    };
    int total = measure();
    while (total > 1150 && px > 19) {  // many categories: smaller pills
        --px;
        pad = std::max(10, pad - 1);
        total = measure();
    }
    int x = (c.width() - total) / 2 + 40;
    const int first_x = x;
    const int h = 46, y = 69 - h / 2;
    for (int i = 0; i < (int)tabs.size(); ++i) {
        const int w = gfx::text_width(Face::SansSemiBold, px, tabs[i]) + 2 * pad;
        const bool on = active == i;
        if (on) c.fill_rounded({x, y, w, h}, h / 2, t.accent);
        gfx::draw_text(c, Face::SansSemiBold, px, x + pad,
                       y + (h - gfx::font_metrics(Face::SansSemiBold, px).line_height) / 2, tabs[i],
                       on ? t.accent_text : t.muted);
        x += w + gap;
    }
    if (tabs.size() > 1) {
        draw_button_glyph(c, kL1, first_x - 54, y + h / 2, 30);
        draw_button_glyph(c, kR1, x - gap + 14, y + h / 2, 30);
    }

    const std::string clock = clock_text();
    const int cw = gfx::text_width(Face::SansSemiBold, 28, clock);
    int rx = c.width() - kMarginX - cw;
    gfx::draw_text(c, Face::SansSemiBold, 28, rx, 69 - gfx::font_metrics(Face::SansSemiBold, 28).line_height / 2, clock,
                   t.text);
    const int running = store::downloads().active();
    if (running > 0) {
        const std::string n = std::to_string(running);
        const int nw = gfx::text_width(Face::SansBold, 24, n);
        rx -= 40 + nw;
        gfx::draw_text(c, Face::SansBold, 24, rx + 30, 69 - gfx::font_metrics(Face::SansBold, 24).line_height / 2, n,
                       t.accent);
        draw_download_icon(c, rx, 69, 26, t.accent);
    } else if (lib::jobs_in_flight() > 0) {
        draw_spinner(c, rx - 36, 69, 11);
    }
}

int draw_button_glyph(Canvas& c, uint32_t button, int x, int cy, int size) {
    const Theme& t = theme();
    const int r = size / 2;
    const int cx = x + r;
    const Color ring = t.text;
    switch (button) {
        case kCross: {
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            const float d = r * 0.42f;
            c.line(cx - d, cy - d, cx + d, cy + d, size / 10.f, gfx::rgb(0x8FB4FF));
            c.line(cx - d, cy + d, cx + d, cy - d, size / 10.f, gfx::rgb(0x8FB4FF));
            return size;
        }
        case kCircle:
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            c.stroke_circle(cx, cy, (int)(r * 0.55f), std::max(2, size / 11), gfx::rgb(0xFF7B88));
            return size;
        case kSquare: {
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            const int s = (int)(r * 0.95f);
            c.stroke_rounded({cx - s / 2, cy - s / 2, s, s}, 2, std::max(2, size / 11), gfx::rgb(0xF5A3D7));
            return size;
        }
        case kTriangle: {
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            const float s = r * 0.55f, th = size / 11.f;
            const Color g = gfx::rgb(0x5FD3B8);
            c.line(cx, cy - s, cx - s * 0.95f, cy + s * 0.65f, th, g);
            c.line(cx - s * 0.95f, cy + s * 0.65f, cx + s * 0.95f, cy + s * 0.65f, th, g);
            c.line(cx + s * 0.95f, cy + s * 0.65f, cx, cy - s, th, g);
            return size;
        }
        default: {
            // Shoulder buttons, Options and the touch pad as labelled keycaps;
            // directions as a small D-pad.
            const char* label = button == kL1 ? "L1" : button == kR1 ? "R1" : button == kL2 ? "L2" : button == kR2 ? "R2"
                              : button == kR3 ? "R3" : button == kOptions ? "OPTIONS" : button == kTouchPad ? "TOUCH PAD"
                              : nullptr;
            if (label) {
                const int px = size * 52 / 100;
                const int w = gfx::text_width(Face::SansBold, px, label) + size * 2 / 3;
                c.fill_rounded({x, cy - r, w, size}, size / 4, gfx::rgb(0x2B3446));
                gfx::draw_text(c, Face::SansBold, px, x + size / 3, cy - gfx::font_metrics(Face::SansBold, px).line_height / 2,
                               label, gfx::rgb(0xDDE3EE));
                return w;
            }
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            const int arm = size / 6, len = (int)(r * 0.75f);
            c.fill({cx - arm / 2, cy - len, arm, len * 2}, ring);
            c.fill({cx - len, cy - arm / 2, len * 2, arm}, ring);
            const Color hi = theme().accent;
            if (button & kUp) c.fill({cx - arm / 2, cy - len, arm, len / 2}, hi);
            if (button & kDown) c.fill({cx - arm / 2, cy + len / 2, arm, len / 2}, hi);
            if (button & kLeft) c.fill({cx - len, cy - arm / 2, len / 2, arm}, hi);
            if (button & kRight) c.fill({cx + len / 2, cy - arm / 2, len / 2, arm}, hi);
            return size;
        }
    }
}

static int glyph_width(uint32_t button, int size) {
    const char* label = button == kOptions ? "OPTIONS" : button == kTouchPad ? "TOUCH PAD"
                      : (button == kL1 || button == kR1 || button == kL2 || button == kR2 || button == kR3) ? "L1"
                      : nullptr;
    return label ? gfx::text_width(Face::SansBold, size * 52 / 100, label) + size * 2 / 3 : size;
}

void draw_footer(Canvas& c, const std::vector<Hint>& hints) {
    const Theme& t = theme();
    c.fill_vgradient({0, kFooterTop - 30, c.width(), c.height() - kFooterTop + 30}, gfx::with_alpha(t.bg, 0),
                     gfx::with_alpha(t.bg, 235));
    const int px = 25, size = 38, cy = kFooterTop + 40;
    int x = c.width() - kMarginX;
    // Right-aligned, in the order given (the first hint is rightmost).
    for (const Hint& h : hints) {
        const int lw = gfx::text_width(Face::Sans, px, h.label);
        x -= lw;
        gfx::draw_text(c, Face::Sans, px, x, cy - gfx::font_metrics(Face::Sans, px).line_height / 2, h.label, t.muted);
        x -= glyph_width(h.button, size) + 12;
        draw_button_glyph(c, h.button, x, cy, size);
        x -= 44;
    }
}

void draw_spinner(Canvas& c, int cx, int cy, int radius) {
    const Theme& t = theme();
    const double phase = (now_ms() % 1200) / 1200.0 * 2 * M_PI;
    for (int i = 0; i < 10; ++i) {
        const double a = phase + i * 2 * M_PI / 10;
        const int alpha = 40 + i * 215 / 9;
        c.fill_circle(cx + (int)(cos(a) * radius), cy + (int)(sin(a) * radius), std::max(2, radius / 5),
                      gfx::with_alpha(t.accent, alpha));
    }
}

void draw_loading(Canvas& c, const Rect& area, const std::string& label) {
    const Theme& t = theme();
    const int cy = area.y + area.h / 2 - 30;
    draw_spinner(c, area.x + area.w / 2, cy, 28);
    const int w = gfx::text_width(Face::Sans, kBodyPx, label);
    gfx::draw_text(c, Face::Sans, kBodyPx, area.x + (area.w - w) / 2, cy + 52, label, t.muted);
}

void draw_message(Canvas& c, const Rect& area, const std::string& title, const std::string& body, bool error) {
    const Theme& t = theme();
    const int width = std::min(area.w, 1100);
    const auto lines = gfx::wrap_text(Face::Sans, kBodyPx, body, width, 10);
    const int line_h = gfx::font_metrics(Face::Sans, kBodyPx).line_height + 6;
    const int height = 70 + (int)lines.size() * line_h;
    int y = area.y + (area.h - height) / 2;
    const int tw = gfx::text_width(Face::SansBold, kHeadPx, title);
    gfx::draw_text(c, Face::SansBold, kHeadPx, area.x + (area.w - tw) / 2, y, title, error ? t.error : t.text);
    y += 70;
    for (const auto& line : lines) {
        const int lw = gfx::text_width(Face::Sans, kBodyPx, line);
        gfx::draw_text(c, Face::Sans, kBodyPx, area.x + (area.w - lw) / 2, y, line, t.muted);
        y += line_h;
    }
}

void draw_progress(Canvas& c, const Rect& r, float value, Color color) {
    const Theme& t = theme();
    c.fill_rounded(r, r.h / 2, gfx::with_alpha(t.text, 40));
    const int w = (int)(r.w * std::max(0.f, std::min(1.f, value)));
    if (w > 0) c.fill_rounded({r.x, r.y, std::max(w, r.h), r.h}, r.h / 2, color ? color : t.accent);
}

void draw_scrollbar(Canvas& c, const Rect& track, int first, int visible, int total) {
    if (total <= visible || total <= 0) return;
    const Theme& t = theme();
    c.fill_rounded(track, track.w / 2, gfx::with_alpha(t.text, 30));
    const int h = std::max(track.w * 3, track.h * visible / total);
    const int y = track.y + (int)((int64_t)(track.h - h) * first / std::max(1, total - visible));
    c.fill_rounded({track.x, y, track.w, h}, track.w / 2, gfx::with_alpha(t.text, 140));
}

int chip_width(const std::string& text, int px) { return gfx::text_width(Face::SansSemiBold, px, text) + px; }

int draw_chip(Canvas& c, int x, int y, const std::string& text, Color fg, Color bg, int px) {
    const int w = chip_width(text, px), h = px * 3 / 2 + 4;
    c.fill_rounded({x, y, w, h}, h / 2, bg);
    gfx::draw_text(c, Face::SansSemiBold, px, x + px / 2, y + (h - gfx::font_metrics(Face::SansSemiBold, px).line_height) / 2,
                   text, fg);
    return w;
}

const char* box_label(BoxStyle style) {
    switch (style) {
    case BoxStyle::PS5: return "PS5";
    case BoxStyle::PS1: return "PS1";
    case BoxStyle::PS2: return "PS2";
    case BoxStyle::PS3: return "PS3";
    case BoxStyle::PSP: return "PSP";
    default: return "PS4";
    }
}

void draw_game_box(Canvas& c, const Rect& r, const std::string& art, const std::string& title, BoxStyle style,
                   bool shadow) {
    const int radius = std::max(4, r.w / 28);
    if (shadow) {
        // A soft drop shadow: three widening, fading layers.
        for (int i = 3; i >= 1; --i)
            c.fill_rounded({r.x - i * 2 + 4, r.y + i * 3 + 4, r.w + i * 4 - 8, r.h}, radius + i * 2,
                           gfx::with_alpha(gfx::rgb(0x000000), 34));
    }
    // Each case's plastic, its band at the top (where the PlayStation logo
    // sits on the real case) and the band's lettering.
    struct Look { Color body, band_top, band_bottom, label, rule; int shine; };
    Look look;
    switch (style) {
    case BoxStyle::PS5:  // white
        look = {gfx::rgb(0xE8EBF0), gfx::rgb(0xFAFBFD), gfx::rgb(0xFAFBFD), gfx::rgb(0x111318), gfx::rgb(0xC9CED8), 60};
        break;
    case BoxStyle::PS1:  // the grey of the original console, black band
        look = {gfx::rgb(0x7A7D85), gfx::rgb(0x2C2E33), gfx::rgb(0x111214), gfx::rgb(0xF2F3F5), gfx::rgb(0x55585E), 60};
        break;
    case BoxStyle::PS2:  // black DVD case
        look = {gfx::rgb(0x121419), gfx::rgb(0x2B2F38), gfx::rgb(0x07080B), gfx::rgb(0xFFFFFF), gfx::rgb(0x3A3F4A), 45};
        break;
    case BoxStyle::PS3:  // smoked Blu-ray case, silver band
        look = {gfx::rgb(0x3A3E46), gfx::rgb(0xE2E5EA), gfx::rgb(0x9DA2AB), gfx::rgb(0x111318), gfx::rgb(0x6E737C), 55};
        break;
    case BoxStyle::PSP:  // clear UMD case, black band
        look = {gfx::rgb(0xE4E7EC), gfx::rgb(0x1C1D21), gfx::rgb(0x050506), gfx::rgb(0xFFFFFF), gfx::rgb(0x8C929C), 80};
        break;
    default:  // PS4: translucent blue plastic
        look = {gfx::rgb(0x1E4FD2), gfx::rgb(0x2A6FE6), gfx::rgb(0x0B3C9E), gfx::rgb(0xFFFFFF), gfx::rgb(0x0B3C9E), 70};
        break;
    }
    c.fill_rounded(r, radius, look.body);
    const int f = std::max(3, r.w * 3 / 100);         // the plastic rim around the insert
    const int band = std::max(14, r.w * 135 / 1000);  // the platform band at the top
    const Rect insert{r.x + f, r.y + f, r.w - 2 * f, r.h - 2 * f};
    const Rect band_r{insert.x, insert.y, insert.w, band};
    c.fill_vgradient(band_r, look.band_top, look.band_bottom);
    // The case and its cover are pictures: their lettering keeps the sans
    // font in every theme (the pixel font does not fit a small case).
    gfx::set_pixel_text(false);
    const int lpx = std::max(10, band * 64 / 100);
    const int ly = band_r.y + (band - gfx::font_metrics(Face::SansBold, lpx).line_height) / 2;
    gfx::draw_text(c, Face::SansBold, lpx, band_r.x + band * 4 / 10, ly, box_label(style), look.label);
    if (style != BoxStyle::PS4) c.fill({band_r.x, band_r.bottom() - 1, band_r.w, 1}, look.rule);

    // The cover art fills the window below the band (square icons lose a little at the sides).
    const Rect window{insert.x, insert.y + band, insert.w, insert.h - band};
    const gfx::Image* img = art.empty() ? nullptr : gfx::image_get(art, window.w, window.h, gfx::Fit::Cover);
    if (img) {
        c.push_clip(window);
        c.draw_image(*img, window.x + (window.w - img->lw()) / 2, window.y + (window.h - img->lh()) / 2);
        c.pop_clip();
    } else {
        // A generated cover: a colour from the title, with the title on it.
        uint32_t hash = 2166136261u;
        for (char ch : title) hash = (hash ^ (uint8_t)ch) * 16777619u;
        static const Color palette[] = {gfx::rgb(0x2C3E66), gfx::rgb(0x4A3B66), gfx::rgb(0x284F4A), gfx::rgb(0x5A3B33),
                                        gfx::rgb(0x23394D), gfx::rgb(0x3F3570), gfx::rgb(0x55354A), gfx::rgb(0x2E4766)};
        const Color base = palette[hash % 8];
        c.fill_vgradient(window, gfx::mix(base, gfx::rgb(0xFFFFFF), 30), base);
        const int px = std::max(14, window.w / 9);
        const int pad = window.w / 10;
        gfx::draw_wrapped(c, Face::SansBold, px, window.x + pad, window.y + window.h / 5, window.w - 2 * pad, title,
                          gfx::rgb(0xF2F4F8), 4);
        if (!art.empty() && gfx::image_state(art) == gfx::ImageState::Loading)
            draw_spinner(c, window.x + window.w / 2, window.bottom() - window.h / 6, std::max(8, window.w / 16));
    }
    gfx::set_pixel_text(arcade());
    // A highlight along the spine side of the plastic.
    c.fill({r.x + f / 2, r.y + radius, std::max(1, f / 2), r.h - 2 * radius}, gfx::with_alpha(gfx::rgb(0xFFFFFF), look.shine));
}

bool ListNav::update(const Input& in, int count, int visible, bool paging) {
    if (count <= 0) return false;
    const int before = index;
    if (in.repeat(kUp)) index = index > 0 ? index - 1 : (in.press(kUp) ? count - 1 : 0);
    if (in.repeat(kDown)) index = index < count - 1 ? index + 1 : (in.press(kDown) ? 0 : count - 1);
    if (paging && in.repeat(kLeft)) index = std::max(0, index - visible);
    if (paging && in.repeat(kRight)) index = std::min(count - 1, index + visible);
    clamp(count, visible);
    return index != before;
}

void ListNav::clamp(int count, int visible) {
    index = std::max(0, std::min(index, count - 1));
    if (index < top) top = index;
    if (index >= top + visible) top = index - visible + 1;
    top = std::max(0, std::min(top, std::max(0, count - visible)));
}

bool GridNav::update(const Input& in, int count, int cols, int visible_rows) {
    if (count <= 0) return false;
    const int before = index;
    if (in.repeat(kLeft) && index % cols > 0) --index;
    if (in.repeat(kRight) && index % cols < cols - 1 && index + 1 < count) ++index;
    if (in.repeat(kUp) && index >= cols) index -= cols;
    if (in.repeat(kDown)) {
        if (index + cols < count) index += cols;
        else if (index / cols < (count - 1) / cols) index = count - 1;  // onto the shorter last row
    }
    // L2 / R2: a page of rows at a time.
    if (in.repeat(kL2)) index = std::max(index % cols, index - cols * visible_rows);
    if (in.repeat(kR2)) index = std::min(count - 1, index + cols * visible_rows);
    clamp(count, cols, visible_rows);
    return index != before;
}

void GridNav::clamp(int count, int cols, int visible_rows) {
    index = std::max(0, std::min(index, count - 1));
    const int row = index / cols;
    if (row < top_row) top_row = row;
    if (row >= top_row + visible_rows) top_row = row - visible_rows + 1;
    top_row = std::max(0, top_row);
}

}  // namespace ui
