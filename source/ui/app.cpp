// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/app.h"
#include "ui/screens.h"
#include "ui/theme.h"

#include <algorithm>

#include "core/log.h"
#include "core/worker.h"
#include "gfx/font.h"
#include "platform/platform.h"
#include "store/downloads.h"
#include "store/installer.h"
#include "store/updater.h"
#include "ui/widgets.h"

#ifdef LIB_HOST
#include "platform/host.h"
#endif

namespace ui {

using namespace platform;

App& app() {
    static App instance;
    return instance;
}

void App::push(std::unique_ptr<Screen> screen) { pending_push_.push_back(std::move(screen)); }
void App::pop() { ++pending_pops_; }
void App::switch_tab(int tab) {
    const int n = (int)tab_names().size();
    pending_root_ = make_tab_screen(n ? ((tab % n) + n) % n : 0);
}

void App::toast(const std::string& message) {
    if (message.empty()) return;
    toast_ = message;
    toast_until_ = now_ms() + 3200;
    request_redraw();
}

Screen* App::below(const Screen* s) {
    for (size_t i = 1; i < stack_.size(); ++i)
        if (stack_[i].get() == s) return stack_[i - 1].get();
    return nullptr;
}

void App::apply_pending() {
    bool changed = false;
    if (pending_root_) {
        stack_.clear();
        stack_.push_back(std::move(pending_root_));
        pending_pops_ = 0;
        changed = true;
    }
    for (; pending_pops_ > 0 && !stack_.empty(); --pending_pops_) {
        stack_.pop_back();
        changed = true;
        if (stack_.empty()) break;
    }
    pending_pops_ = 0;
    for (auto& s : pending_push_) {
        stack_.push_back(std::move(s));
        changed = true;
    }
    const bool pushed = !pending_push_.empty();
    pending_push_.clear();
    if (changed) {
        input_.reset();
        if (!pushed && !stack_.empty()) stack_.back()->resumed();
        request_redraw();
    }
    if (stack_.empty()) stack_.push_back(make_tab_screen(0));
}

void App::draw() {
    gfx::Canvas c(canvas(), frame_width(), frame_height(), kWidth, kHeight);
    // Draw from the topmost non-overlay screen upwards.
    size_t first = stack_.size() - 1;
    while (first > 0 && stack_[first]->overlay()) --first;
    Screen* base = stack_[first].get();
    if (!base->own_background()) draw_background(c);
    base->draw(c);
    draw_header(c, base->tab());
    if (first == stack_.size() - 1) draw_footer(c, base->hints());
    for (size_t i = first + 1; i < stack_.size(); ++i) stack_[i]->draw(c);
    if (stack_.size() - 1 > first) draw_footer(c, stack_.back()->hints());

    if (arcade()) {
        // Scanlines: every fourth line a little darker, like a tube screen.
        const gfx::Color dark = gfx::with_alpha(gfx::rgb(0x000000), 105);
        for (int y = 0; y < kHeight; y += 3) c.fill({0, y, kWidth, 1}, dark);
    }

    if (!toast_.empty() && now_ms() < toast_until_) {
        const Theme& t = theme();
        const int px = kBodyPx;
        const std::string text = gfx::ellipsize(gfx::Face::Sans, px, toast_, 1500);
        const int w = gfx::text_width(gfx::Face::Sans, px, text) + 72;
        const gfx::Rect r{(kWidth - w) / 2, kFooterTop - 110, w, 72};
        c.fill_rounded(r, 36, gfx::with_alpha(t.surface_hi, 245));
        gfx::draw_text(c, gfx::Face::Sans, px, r.x + 36,
                       r.y + (72 - gfx::font_metrics(gfx::Face::Sans, px).line_height) / 2, text, t.text);
    }
}

void App::run() {
    switch_tab(0);
    apply_pending();
    bool toast_visible = false, blink_was = false;
    uint64_t last_progress = 0;
    // How long frames take, for the debug log (a slow screen shows up here).
    uint64_t stats_since = now_ms(), draw_ms = 0, present_ms = 0, worst_ms = 0;
    int frames = 0;
    while (!quit_) {
#ifdef LIB_HOST
        if (host::quit_requested()) break;
#endif
        const Input in = input_.read();
        const bool worked = lib::run_completions();
        store::refresh_installed();
        for (const std::string& m : store::downloads().tick()) toast(m);
        for (const std::string& m : store::installer_tick()) toast(m);
        for (const std::string& m : store::update_blocker_tick()) toast(m);
        // A newer FPKGi+: said once, wherever the user is.
        static bool update_told = false;
        if (!update_told && store::updater::state() == store::updater::State::Available) {
            update_told = true;
            toast("FPKGi+ " + store::updater::available_version() + " is available: Settings > System > Software update");
        }
        Screen* top = stack_.back().get();

        // L1 / R1 change tab on any tab's root screen.
        const int tabs = (int)tab_names().size();
        if (stack_.size() == 1 && top->tab() >= 0 && tabs > 1 && (in.press(kL1) || in.press(kR1))) {
            switch_tab(top->tab() + (in.press(kR1) ? 1 : -1));
        } else {
            top->update(in);
        }
        apply_pending();

        const bool toast_now = !toast_.empty() && now_ms() < toast_until_;
        // A running download moves its numbers: redraw twice a second.
        bool progress = false;
        const auto update_state = store::updater::state();
        const bool updating = update_state == store::updater::State::Checking ||
                              update_state == store::updater::State::Downloading;
        if ((store::downloads().active() > 0 || updating) && now_ms() - last_progress >= 500) {
            last_progress = now_ms();
            progress = true;
        }
        // The update check finishing (or failing) changes what Settings shows.
        static store::updater::State last_update_state = update_state;
        if (update_state != last_update_state) {
            last_update_state = update_state;
            progress = true;
        }
        // The arcade cursor blinks: one redraw each time it changes.
        const bool blink_now = arcade() && blink_on();
        const bool blinked = arcade() && blink_now != blink_was;
        blink_was = blink_now;
        const bool dirty = take_redraw() || worked || blinked || in.pressed || in.repeated || in.held || in.rx || in.ry ||
                           in.lx || in.ly || stack_.back()->animating() || toast_now != toast_visible ||
                           lib::jobs_in_flight() > 0 || progress;
        toast_visible = toast_now;
        if (dirty) {
            const uint64_t t0 = now_ms();
            draw();
            const uint64_t t1 = now_ms();
            present();
            const uint64_t t2 = now_ms();
            draw_ms += t1 - t0;
            present_ms += t2 - t1;
            worst_ms = std::max(worst_ms, t1 - t0);
            ++frames;
        } else {
            sleep_ms(8);  // the controller is read again soon: a press is seen within a frame
        }
        if (now_ms() - stats_since >= 20000) {
            if (frames > 0)
                LOGD("frames: %d in 20 s, drawing %.1f ms each (worst %d ms), showing %.1f ms each", frames,
                     (double)draw_ms / frames, (int)worst_ms, (double)present_ms / frames);
            stats_since = now_ms();
            draw_ms = present_ms = worst_ms = 0;
            frames = 0;
        }
    }
}

}  // namespace ui
