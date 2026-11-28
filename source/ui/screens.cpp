// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The store: one tab per category, a grid of game boxes with the focused
// title's details beside it, the details page, and the Downloads tab.
#include "ui/screens.h"

#include <math.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "core/worker.h"
#include "gfx/image_cache.h"
#include "net/archive.h"
#include "platform/platform.h"
#include "store/covers.h"
#include "store/service.h"
#include "store/downloads.h"
#include "store/installer.h"
#include "store/metadata.h"
#include "store/homebrew_updates.h"
#include "store/updater.h"
#include "ui/app.h"
#include "ui/overlays.h"
#include "ui/widgets.h"

#ifdef LIB_VERSION
#define LIB_VERSION_STR LIB_VERSION
#else
#define LIB_VERSION_STR "dev"
#endif

namespace ui {
void choose_usb_drive(std::function<void()> selected);

std::string download_location_name(const std::string& path);  // "Console", "USB drive 0"

using gfx::Canvas;
using gfx::Color;
using gfx::Face;
using gfx::Rect;
using store::Category;
using store::Item;
using namespace platform;

namespace {

// The order of the tabs; empty categories get no tab. PS5 is never shown:
// a PS4 cannot install PS5 packages.
const Category kTabOrder[] = {Category::Games,   Category::Updates,  Category::DLC,    Category::Apps,
                              Category::Homebrew, Category::Emulators, Category::Themes, Category::Demos,
                              Category::PS1,     Category::PS2,      Category::PSP};

// The case a title is drawn in: its list's platform, or PS5 for a PS5
// package found in another list, otherwise PS4.
BoxStyle box_style(const Item& item) {
    switch (item.category) {
    case Category::PS1: return BoxStyle::PS1;
    case Category::PS2: return BoxStyle::PS2;
    case Category::PSP: return BoxStyle::PSP;
    default: return item.ps5 ? BoxStyle::PS5 : BoxStyle::PS4;
    }
}

bool g_loading = false;
std::string g_search;  // shared by every tab
// The Installed tab's "Show" filter: one list (Games, Homebrew...) or, with
// Category::Count, all of them. For this run of the app.
Category g_installed_show = Category::Count;

// Lists whose installed titles the Installed tab shows.
bool installed_tab_list(Category c) {
    return c != Category::Updates && c != Category::DLC && c != Category::Themes && c != Category::PS5;
}

void forget_file_states();

// Queues a download (the primary "Download" action), with a toast.
void get_item_direct(const Item& item) {
    const auto queue = [item] {
        if (store::downloads().add(item)) app().toast("Downloading " + item.name);
        else app().toast("Could not queue " + item.name + ". Check its download destination.");
    };
    for (const auto& job : store::downloads().snapshot()) {
        if (job.item.url == item.url && (job.state == store::JobState::Paused ||
            job.state == store::JobState::Queued || job.state == store::JobState::Downloading)) {
            queue();
            return;
        }
    }
    auto cat = store::catalog();
    if (cat->config().download_to_usb &&
        !store::resolve_download_destination(cat->config(), store::connected_usb_drives())) {
        choose_usb_drive(queue);
        return;
    }
    queue();
}

std::vector<Category> visible_categories() {
    std::vector<Category> out;
    const auto cat = store::catalog();
    if (!cat->loaded()) return out;
    for (Category c : kTabOrder)
        if (!cat->items(c).empty()) out.push_back(c);
    return out;
}

std::string size_text(uint64_t bytes) { return lib::format_bytes(bytes); }

std::string version_text(const Item& item) {
    std::string v = item.version;
    if (v.empty() || v == "?.??") return "";
    // "01.04" -> "1.04"
    if (v.size() > 1 && v[0] == '0' && v[1] != '.') v = v.substr(1);
    return "v" + v;
}

std::string duration_text(double seconds) {
    if (seconds <= 0 || seconds > 360000) return "";
    const int s = (int)seconds;
    char buf[32];
    if (s >= 3600) snprintf(buf, sizeof buf, "%d h %02d min left", s / 3600, (s % 3600) / 60);
    else if (s >= 60) snprintf(buf, sizeof buf, "%d min left", s / 60);
    else snprintf(buf, sizeof buf, "%d s left", s);
    return buf;
}

// What the console and the download folder say about a title. InstallIssue:
// an install that stalled or failed and still holds its PKG (installer.h).
enum class Status { None, Installed, Installing, InstallIssue, Removing, Downloaded, Partial, Queued, Downloading, Paused };

struct StatusInfo {
    Status status = Status::None;
    float progress = 0;  // for Downloading / Paused
};

std::string item_install_path(const Item& item, const std::vector<store::Job>& jobs);

StatusInfo status_of(const Item& item, const std::vector<store::Job>& jobs) {
    StatusInfo s;
    if (store::app_removal_pending(item)) { s.status = Status::Removing; return s; }
    const std::string install_path = item_install_path(item, jobs);
    if (store::installation_uses_file(install_path)) {
        const auto attention = store::installation_progress(install_path).attention;
        // Verified but its task is not released: the title is installed, and
        // is shown so; only the PKG waits (see the Downloads row).
        if (attention != store::InstallAttention::ReleaseStalled) {
            s.status = attention == store::InstallAttention::None ? Status::Installing : Status::InstallIssue;
            return s;
        }
    }
    for (const auto& j : jobs) {
        if (j.item.url != item.url) continue;
        if (j.state == store::JobState::Downloading) {
            s.status = Status::Downloading;
            s.progress = j.total ? (float)((double)j.done / (double)j.total) : 0.f;
            return s;
        }
        if (j.state == store::JobState::Paused) {
            s.status = Status::Paused;
            s.progress = j.total ? (float)((double)j.done / (double)j.total) : 0.f;
            return s;
        }
        if (j.state == store::JobState::Queued) {
            s.status = Status::Queued;
            return s;
        }
    }
    if (store::is_installed(item)) s.status = Status::Installed;
    return s;
}

// The download folder's state is a file check: cached per title, refreshed
// when screens are resumed and downloads finish.
std::map<std::string, store::Downloads::FileState> g_file_states;
store::Downloads::FileState file_state(const Item& item) {
    static uint64_t revision = 0;
    const uint64_t current = store::downloads().revision();
    if (revision != current) { g_file_states.clear(); revision = current; }
    auto found = g_file_states.find(item.url);
    if (found != g_file_states.end()) return found->second;
    const auto s = store::Downloads::file_state(item);
    g_file_states[item.url] = s;
    return s;
}
void forget_file_states() { g_file_states.clear(); }

StatusInfo full_status(const Item& item, const std::vector<store::Job>& jobs) {
    StatusInfo s = status_of(item, jobs);
    if (s.status != Status::None) return s;
    const auto f = file_state(item);
    if (f == store::Downloads::FileState::Complete) s.status = Status::Downloaded;
    else if (f == store::Downloads::FileState::Partial) s.status = Status::Partial;
    return s;
}

const char* status_label(Status s) {
    switch (s) {
        case Status::Installed: return "Installed";
        case Status::Installing: return "Installing";
        case Status::InstallIssue: return "Install needs attention";
        case Status::Removing: return "Removing...";
        case Status::Downloaded: return "Downloaded";
        case Status::Partial: return "Partly downloaded";
        case Status::Queued: return "Queued";
        case Status::Downloading: return "Downloading";
        case Status::Paused: return "Paused";
        default: return "";
    }
}

// One line for a row. Installing and its progress while it runs; Installed
// only once verified; the stalled states say what is kept.
std::string install_progress_text(const store::InstallTaskProgress& progress) {
    switch (progress.attention) {
        case store::InstallAttention::VerificationStalled: return "Downloaded - installation not verified; PKG kept";
        case store::InstallAttention::ReleaseStalled: return "Installed - install task not released; PKG kept";
        case store::InstallAttention::FailedTaskHeld: return "Downloaded - installation failed; task not released; PKG kept";
        default: break;
    }
    if (progress.state == store::InstallTaskState::Complete) return "Installed";
    if (progress.message == "Finalizing installation...") return progress.message;
    if (progress.percent < 0) return progress.state == store::InstallTaskState::Unknown ? "Checking installation..." : "Preparing installation...";
    std::string text = "Installing " + std::to_string(progress.percent) + "%";
    if (progress.total) text += " - " + lib::format_bytes(std::min(progress.done, progress.total)) + " of " + lib::format_bytes(progress.total);
    return text;
}

std::string item_install_path(const Item& item, const std::vector<store::Job>& jobs) {
    const std::string path = store::package_path(item);
    if (store::installation_uses_file(path)) return path;
    for (const auto& job : jobs)
        if (job.item.title_id == item.title_id && store::installation_uses_file(job.path)) return job.path;
    return path;
}

Color status_color(Status s) {
    const Theme& t = theme();
    switch (s) {
        case Status::Installed: return t.success;
        case Status::InstallIssue:
        case Status::Downloaded: return t.warning;
        case Status::Partial: return gfx::rgb(0xE59A5C);
        case Status::Paused: return t.muted;
        default: return t.accent;
    }
}

// A small round badge on a box's corner: a tick (installed), an arrow
// (downloaded), dots (partly downloaded or queued), pause bars (paused).
void draw_badge(Canvas& c, int cx, int cy, int r, Status s) {
    if (s == Status::None || s == Status::Downloading) return;
    const Color col = status_color(s);
    c.fill_circle(cx, cy, r + 3, gfx::rgb(0x0A0F1C));
    c.fill_circle(cx, cy, r, col);
    const Color ink = gfx::rgb(0x0A0F1C);
    const float th = std::max(2.f, r / 4.f);
    if (s == Status::Installed) {
        c.line(cx - r * 0.45f, cy + r * 0.02f, cx - r * 0.1f, cy + r * 0.38f, th, ink);
        c.line(cx - r * 0.1f, cy + r * 0.38f, cx + r * 0.5f, cy - r * 0.35f, th, ink);
    } else if (s == Status::Downloaded) {
        c.line(cx, cy - r * 0.5f, cx, cy + r * 0.3f, th, ink);
        c.line(cx - r * 0.35f, cy, cx, cy + r * 0.35f, th, ink);
        c.line(cx + r * 0.35f, cy, cx, cy + r * 0.35f, th, ink);
    } else if (s == Status::Paused) {
        c.fill({cx - (int)(r * 0.32f), cy - (int)(r * 0.4f), std::max(2, (int)(r * 0.22f)), (int)(r * 0.8f)}, ink);
        c.fill({cx + (int)(r * 0.1f), cy - (int)(r * 0.4f), std::max(2, (int)(r * 0.22f)), (int)(r * 0.8f)}, ink);
    } else {
        for (int i = -1; i <= 1; ++i) c.fill_circle(cx + (int)(i * r * 0.42f), cy, std::max(2, r / 6), ink);
    }
}

// What can be done about an install that stalled or failed while the console
// still holds its task (store/installer.h). Nothing here deletes the PKG.
void open_install_recovery(const std::string& path, const std::string& name) {
    const auto progress = store::installation_progress(path);
    if (!store::installation_uses_file(path) || progress.attention == store::InstallAttention::None) return;
    const auto report = [](const std::string& message) {
        forget_file_states();
        app().toast(message);
    };
    std::vector<MenuItem> items;
    items.push_back({progress.attention == store::InstallAttention::FailedTaskHeld ? "Try releasing the task again" : "Check again",
                     nullptr, [path, report] {
                         std::string message;
                         store::recheck_installation(path, message);
                         report(message);
                     }, nullptr});
    if (progress.attention == store::InstallAttention::VerificationStalled)
        items.push_back({"Stop waiting", nullptr, [path, name, report] {
            confirm("Stop waiting for " + name + "?",
                    "FPKGi+ asks the console to release this install task. The title is not marked as installed and the "
                    "PKG is kept, so Install can be tried again.",
                    "Stop waiting", [path, report] {
                        std::string message;
                        store::abandon_installation(path, false, message);
                        report(message);
                    });
        }, nullptr});
    MenuItem forget{"Forget install task", nullptr, [path, name, report] {
        confirm("Forget the install task for " + name + "?",
                "Only after its entry is gone from Notifications > Downloads on the console (removed, or finished there). "
                "FPKGi+ stops tracking the task and protecting the PKG. The PKG itself is kept.",
                "Forget task", [path, report] {
                    std::string message;
                    store::abandon_installation(path, true, message);
                    report(message);
                });
    }, nullptr};
    forget.destructive = true;
    items.push_back(forget);
    open_menu("Resolve installation", std::move(items), progress.message);
}

// The focused title's cover, enlarged until it is a blur, behind the page.
class Backdrop {
public:
    // The blurred cover under two gradients used to be blended every frame
    // (millions of pixels): it is composed once per cover and theme, then
    // each frame is one plain copy.
    void draw(Canvas& c, const std::string& art) {
        const bool plain = arcade() || art.empty();
        const std::string want = plain ? std::string(arcade() ? "arcade" : "plain") : art;
        if (want != source_ || c.pixel_width() != frame_.w) {
            const gfx::Image* small = plain ? nullptr : gfx::image_get(art, 32, 18, gfx::Fit::Cover);
            if (plain || small || frame_.empty()) {
                frame_.w = c.pixel_width();
                frame_.h = c.pixel_height();
                frame_.px.assign((size_t)frame_.w * frame_.h, 0xFF000000u);
                frame_.opaque = true;
                frame_.density = c.scale();
                Canvas on(frame_.px.data(), frame_.w, frame_.h, c.width(), c.height());
                draw_background(on);
                if (small) {
                    // Bilinear enlargement of a tiny copy: a soft blur for free.
                    gfx::Image blur = gfx::scale_image(*small, frame_.w, frame_.h);
                    blur.density = c.scale();
                    on.draw_image(blur, 0, 0, 80);
                    const Theme& t = theme();
                    // Darker towards the bottom, where text sits.
                    on.fill_vgradient({0, 0, c.width(), c.height()}, gfx::with_alpha(t.bg, 120), gfx::with_alpha(t.bg, 225));
                }
                if (plain || small) source_ = want;  // else: the cover is still loading, asked again next frame
            }
        }
        c.draw_image(frame_, 0, 0);
    }

private:
    std::string source_;
    gfx::Image frame_;
};

// ---- the store tab -----------------------------------------------------------------

constexpr int kBoxW = 150;
constexpr int kColPitch = 190;
constexpr int kRowPitch = 262;
constexpr int kCols = 6;
constexpr int kRows = 3;
constexpr int kGridX = kMarginX;
constexpr int kGridY = 222;
constexpr int kPanelX = kMarginX + kCols * kColPitch + 24;  // 1260

class StoreScreen : public Screen {
public:
    StoreScreen(int tab, Category category, bool installed = false)
        : tab_(tab), category_(category), installed_(installed) { rebuild(); }

    int tab() const override { return tab_; }
    bool own_background() const override { return true; }
    bool animating() const override { return g_loading; }

    std::vector<Hint> hints() const override {
        return {{kCircle, "Quit"}, {kTriangle, "Sort & filter"}, {kSquare, "Search"}, {kOptions, "Queue"},
                {kCross, "Open"}};
    }

    void resumed() override {
        forget_file_states();
        store::refresh_installed();
        rebuild();
    }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            const int running = store::downloads().active();
            confirm("Quit FPKGi+?",
                    running ? "Downloads stop now and continue the next time FPKGi+ is opened. Installs the console "
                              "has started carry on."
                            : "",
                    "Quit", [] { app().quit(); });
            return;
        }
        if (in.press(kTriangle)) {
            open_options_menu(category_, installed_);
            return;
        }
        if (in.press(kSquare) || in.press(kTouchPad)) {
            open_keyboard("Search by name or title ID", g_search, KeyboardKind::Text, [](const std::string& text) {
                g_search = lib::trim(text);
            });
            return;
        }
        // The Installed tab follows installs and removals as they happen.
        if (installed_ && built_revision_ != store::installed_revision()) rebuild();
        if (order_.empty()) return;
        nav_.update(in, (int)order_.size(), cols(), rows());
        if (arcade()) {  // a list: left and right jump a page
            if (in.repeat(kLeft)) nav_.index = std::max(0, nav_.index - rows());
            if (in.repeat(kRight)) nav_.index = std::min((int)order_.size() - 1, nav_.index + rows());
            nav_.clamp((int)order_.size(), cols(), rows());
        }
        // X opens the title's page, where Download / Install / Pause live.
        if (in.press(kCross)) app().push(make_details_screen(current()));
        // OPTIONS queues a download straight away, so a pile of titles can be
        // sent to the background queue without opening each one.
        else if (in.press(kOptions)) {
            const Item& it = current();
            if (store::is_installed(it)) app().toast(it.name + " is already installed.");
            else if (store::Downloads::file_state(it) == store::Downloads::FileState::Complete)
                app().toast(store::install_local(store::package_path(it), it).message);
            else get_item_direct(it);
        }
    }

    void draw(Canvas& c) override {
        const Theme& t = theme();
        const auto cat = store::catalog();
        backdrop_.draw(c, order_.empty() ? "" : store::cover_source(current()));
        if (!cat->loaded()) {
            draw_loading(c, {0, kContentTop, c.width(), kFooterTop - kContentTop}, "Reading your content lists\xE2\x80\xA6");
            return;
        }
        if (cat->total() == 0) {
            std::string body =
                "Add content list URLs in Settings > Content sources, or place GAMES.json and other category files in " +
                lib::join_path(platform::data_root(), "ContentJSONs") + ".";
            for (const auto& p : cat->problems()) body += "  " + p + ".";
            draw_message(c, {0, kContentTop, c.width(), kFooterTop - kContentTop}, "No content yet", body);
            return;
        }

        // Title row: category, count, search, sort.
        const int y0 = 150;
        int x = kGridX;
        std::string heading = installed_ ? "Installed" : store::category_name(category_);
        if (installed_ && g_installed_show != Category::Count) heading += std::string(" / ") + store::category_name(g_installed_show);
        if (!installed_ && category_ == Category::Homebrew && cat->config().homebrew_category != "All")
            heading += " / " + cat->config().homebrew_category;
        x += gfx::draw_text(c, Face::SansBold, 40, x, y0, heading, t.text) + 20;
        const std::string count = lib::format_count(order_.size()) + (order_.size() == 1 ? " title" : " titles");
        gfx::draw_text(c, Face::Sans, kSmallPx, x, y0 + 14, count, t.muted);
        x += gfx::text_width(Face::Sans, kSmallPx, count) + 20;
        if (!g_search.empty())
            draw_chip(c, x, y0 + 8, "Search: " + gfx::ellipsize(Face::SansSemiBold, 22, g_search, 300), t.accent_text,
                      t.accent);
        const std::string sort = sort_label();
        gfx::draw_text(c, Face::Sans, kSmallPx, kPanelX - 40 - gfx::text_width(Face::Sans, kSmallPx, sort), y0 + 14, sort,
                       t.faint);

        if (order_.empty()) {
            if (installed_ && g_search.empty()) {
                draw_message(c, {kGridX, kGridY, kCols * kColPitch, kRows * kRowPitch}, "Nothing installed yet",
                             g_installed_show == Category::Count
                                 ? "Games and apps from your content lists appear here once they are installed on the console."
                                 : std::string("No installed titles from ") + store::category_name(g_installed_show) +
                                       ". Change Show in Sort & filter.");
                return;
            }
            draw_message(c, {kGridX, kGridY, kCols * kColPitch, kRows * kRowPitch}, "Nothing matches",
                         g_search.empty() ? "No titles here in the regions chosen in Options."
                                          : "No title's name or title ID starts with \"" + g_search + "\".");
            return;
        }

        const auto jobs = store::downloads().snapshot();
        if (arcade()) {
            draw_arcade_list(c, jobs);
            draw_panel_for(c, current(), full_status(current(), jobs));
            return;
        }
        const int first = nav_.top_row * kCols;
        const int last = std::min((int)order_.size(), first + kCols * kRows);
        const int box_h = box_height(kBoxW);
        // Covers of the next rows start loading before they are reached.
        for (int i = last; i < std::min((int)order_.size(), last + kCols * 2); ++i) {
            const Item& it = at(i);
            const int bw = kBoxW - 2 * (kBoxW * 3 / 100);
            if (!it.cover_url.empty())
                gfx::image_get(store::cover_source(it), bw, box_h - bw * 135 / 1000, gfx::Fit::Cover);
        }
        Rect focus_rect;
        for (int i = first; i < last; ++i) {
            const Item& it = at(i);
            const int col = (i - first) % kCols, row = (i - first) / kCols;
            const Rect r{kGridX + col * kColPitch, kGridY + row * kRowPitch, kBoxW, box_h};
            const bool focused = i == nav_.index;
            if (focused) {
                focus_rect = r;
                continue;  // drawn last, larger, over its neighbours
            }
            draw_tile(c, r, it, full_status(it, jobs), false);
        }
        if (!focus_rect.empty()) {
            const Item& it = current();
            const int grow = 10;
            const Rect r{focus_rect.x - grow, focus_rect.y - grow * 5 / 4, focus_rect.w + 2 * grow,
                         box_height(focus_rect.w + 2 * grow)};
            draw_tile(c, r, it, full_status(it, jobs), true);
        }
        draw_scrollbar(c, {kPanelX - 22, kGridY, 6, kRows * kRowPitch - 40}, nav_.top_row,
                       kRows, ((int)order_.size() + kCols - 1) / kCols);
        draw_panel_for(c, current(), full_status(current(), jobs));
    }

private:
    const Item& at(int i) const { return store::catalog()->items(order_[i].first)[order_[i].second]; }
    const Item& current() const { return at(nav_.index); }

    std::string sort_label() const {
        const auto& cfg = store::catalog()->config();
        const char* by = cfg.sort == "size" ? "size" : cfg.sort == "region" ? "region" : cfg.sort == "titleid" ? "title ID" : "name";
        return std::string("Sorted by ") + by + (cfg.ascending ? "" : ", reversed");
    }

    void rebuild() {
        const auto cat = store::catalog();
        std::string keep = order_.empty() || nav_.index >= (int)order_.size() ? "" : current().url;
        order_.clear();
        if (!cat->loaded()) return;
        const auto& cfg = cat->config();
        const std::string q = lib::lower(g_search);
        const auto matches = [&](const Item& it) {
            return q.empty() || lib::icontains(it.name, q) || lib::starts_with(lib::lower(it.title_id), q);
        };
        if (installed_) {
            // Every installed game or app from the lists, once per title ID.
            // Region and homebrew filters do not hide what is on the console.
            // Updates and add-ons belong to their game (its page lists them).
            built_revision_ = store::installed_revision();
            std::set<std::string> seen;
            for (Category c : kTabOrder) {
                if (!installed_tab_list(c)) continue;
                if (g_installed_show != Category::Count && c != g_installed_show) continue;
                const auto& items = cat->items(c);
                for (int i = 0; i < (int)items.size(); ++i) {
                    const Item& it = items[i];
                    if (it.ps5 || it.title_id.empty() || !store::is_installed(it) || !matches(it)) continue;
                    if (seen.insert(it.title_id).second) order_.push_back({c, i});
                }
            }
        } else {
            const auto& items = cat->items(category_);
            for (int i = 0; i < (int)items.size(); ++i) {
                const Item& it = items[i];
                if (category_ == Category::Homebrew && cfg.homebrew_category != "All" &&
                    it.homebrew_category != cfg.homebrew_category) continue;
                if (!store::region_shown(cfg, it.region)) continue;
                if (!matches(it)) continue;
                order_.push_back({category_, i});
            }
        }
        using Ref = std::pair<Category, int>;
        const auto item_of = [&](const Ref& r) -> const Item& { return cat->items(r.first)[r.second]; };
        auto key_less = [&](const Ref& a, const Ref& b) {
            const Item& x = item_of(a);
            const Item& y = item_of(b);
            if (cfg.sort == "size" && x.size != y.size) return x.size < y.size;
            if (cfg.sort == "region" && x.region != y.region) return x.region < y.region;
            if (cfg.sort == "titleid" && x.title_id != y.title_id) return x.title_id < y.title_id;
            const std::string xn = lib::lower(x.name), yn = lib::lower(y.name);
            if (xn != yn) return lib::natural_less(xn, yn);
            return a < b;
        };
        std::stable_sort(order_.begin(), order_.end(), [&](const Ref& a, const Ref& b) {
            return cfg.ascending ? key_less(a, b) : key_less(b, a);
        });
        nav_.index = 0;
        for (int i = 0; i < (int)order_.size(); ++i)
            if (item_of(order_[i]).url == keep) nav_.index = i;
        nav_.top_row = std::max(0, nav_.index / cols() - 1);
        nav_.clamp((int)order_.size(), cols(), rows());
        built_search_ = g_search;
    }

    static constexpr int kListRows = 12;
    static constexpr int kListPitch = 58;
    int cols() const { return arcade() ? 1 : kCols; }
    int rows() const { return arcade() ? kListRows : kRows; }

    // The arcade theme's picker: a double-framed black box of numbered names in
    // the pixel font; the cursor is a solid bar with a blinking arrow.
    void draw_arcade_list(Canvas& c, const std::vector<store::Job>& jobs) {
        const Theme& t = theme();
        const int x = kGridX, w = kPanelX - 40 - kGridX, y0 = kGridY - 12, h = kListRows * kListPitch + 28;
        c.fill({x, y0, w, h}, gfx::rgb(0x000000));
        c.stroke_rounded({x, y0, w, h}, 0, 4, t.divider);
        c.stroke_rounded({x + 8, y0 + 8, w - 16, h - 16}, 0, 2, t.accent);
        const int first = nav_.top_row, last = std::min((int)order_.size(), first + kListRows);
        const int line = gfx::font_metrics(Face::Pixel, 24).line_height;
        const int small = gfx::font_metrics(Face::Pixel, 16).line_height;
        for (int i = first; i < last; ++i) {
            const Item& it = at(i);
            const bool on = i == nav_.index;
            const Rect row{x + 14, y0 + 14 + (i - first) * kListPitch, w - 28, kListPitch - 4};
            if (on) c.fill(row, t.accent);
            const int ty = row.y + (row.h - line) / 2;
            if (on && blink_on()) gfx::draw_text(c, Face::Pixel, 24, row.x + 12, ty, ">", t.focus);
            char number[16];
            snprintf(number, sizeof number, "%03d", i + 1);
            gfx::draw_text(c, Face::Pixel, 24, row.x + 48, ty, number, on ? t.focus : t.faint);
            const StatusInfo s = full_status(it, jobs);
            std::string right = size_text(it.size);
            if (s.status == Status::Downloading) right = std::to_string((int)lround(s.progress * 100)) + "%";
            else if (s.status != Status::None) right = status_label(s.status);
            for (char& ch : right) ch = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
            const int rw = gfx::text_width(Face::Pixel, 16, right);
            gfx::draw_text(c, Face::Pixel, 16, row.right() - 16 - rw, row.y + (row.h - small) / 2, right,
                           on ? t.accent_text : s.status != Status::None ? status_color(s.status) : t.muted);
            std::string name = it.name;
            for (char& ch : name) ch = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
            gfx::draw_text(c, Face::Pixel, 24, row.x + 144, ty,
                           gfx::ellipsize(Face::Pixel, 24, name, row.w - 144 - rw - 40), on ? t.accent_text : t.text);
        }
        draw_scrollbar(c, {kPanelX - 22, y0, 6, h}, nav_.top_row, kListRows, (int)order_.size());
    }

    void draw_tile(Canvas& c, const Rect& r, const Item& it, const StatusInfo& s, bool focused) {
        const Theme& t = theme();
        if (focused) {
            // A glow, then the ring, around the case.
            c.fill_rounded({r.x - 14, r.y - 14, r.w + 28, r.h + 28}, 20, gfx::with_alpha(t.accent, 50));
            c.stroke_rounded({r.x - 7, r.y - 7, r.w + 14, r.h + 14}, 12, 4, t.focus);
        }
        draw_game_box(c, r, store::cover_source(it), it.name, box_style(it), !focused);
        draw_badge(c, r.right() - 14, r.y + 14 + r.w * 135 / 1000, 14, s.status);
        if (s.status == Status::Downloading || s.status == Status::Queued)
            draw_progress(c, {r.x + 10, r.bottom() - 18, r.w - 20, 8}, s.progress);
        const int px = focused ? 24 : 22;
        const Face face = focused ? Face::SansBold : Face::SansSemiBold;
        const int ty = r.bottom() + (focused ? 14 : 12);
        gfx::draw_text(c, face, px, r.x, ty, gfx::ellipsize(face, px, it.name, kColPitch - 14),
                       focused ? t.text : t.muted);
    }

    void draw_panel_for(Canvas& c, const Item& it, const StatusInfo& s) {
        const Theme& t = theme();
        const Rect panel{kPanelX, kGridY - 30, 1920 - kMarginX - kPanelX, kRows * kRowPitch + 10};
        c.fill_rounded(panel, 22, gfx::with_alpha(t.surface, 205));
        const int bw = 230, bh = box_height(bw);
        const Rect box{panel.x + (panel.w - bw) / 2, panel.y + 34, bw, bh};
        draw_game_box(c, box, store::cover_source(it), it.name, box_style(it));
        draw_badge(c, box.right() - 18, box.y + 18 + bw * 135 / 1000, 18, s.status);
        int y = box.bottom() + 26;
        const int tx = panel.x + 34, tw = panel.w - 68;
        y += gfx::draw_wrapped(c, Face::SansBold, 32, tx, y, tw, it.name, t.text, 2) + 12;
        int cx = tx;
        const Color chip_bg = gfx::with_alpha(t.text, 30);
        cx += draw_chip(c, cx, y, it.region, t.text, chip_bg) + 10;
        const std::string v = version_text(it);
        if (!v.empty()) cx += draw_chip(c, cx, y, v, t.text, chip_bg) + 10;
        cx += draw_chip(c, cx, y, size_text(it.size), t.text, chip_bg) + 10;
        y += 50;
        std::string line = it.title_id;
        if (!it.min_fw.empty() && it.min_fw != "?.??") line += "  \xC2\xB7  FW " + it.min_fw + "+";
        gfx::draw_text(c, Face::Sans, kSmallPx, tx, y, line, t.muted);
        y += 36;
        const std::string date = it.category == Category::Homebrew ? it.latest_release_date : it.release_date;
        if (!date.empty()) {
            gfx::draw_text(c, Face::Sans, kSmallPx, tx, y,
                (it.category == Category::Homebrew ? "Latest release " : "Released ") + date, t.faint);
            y += 36;
        }
        y += 8;
        if (s.status != Status::None) {
            std::string label = status_label(s.status);
            if (s.status == Status::Downloading) label += " " + std::to_string((int)lround(s.progress * 100)) + "%";
            c.fill_circle(tx + 8, y + 15, 7, status_color(s.status));
            gfx::draw_text(c, Face::SansSemiBold, kSmallPx, tx + 26, y, label, status_color(s.status));
            y += 40;
        }
        if (it.category == Category::Homebrew) {
            const int lines = std::min(4, std::max(0, (panel.bottom() - 110 - y) / 30));
            if (lines > 0 && !it.description.empty())
                y += gfx::draw_wrapped(c, Face::Sans, 24, tx, y, tw, it.description, t.muted, lines) + 12;
            if (!it.developer.empty() && y + 30 < panel.bottom() - 70)
                gfx::draw_text(c, Face::Sans, 22, tx, y,
                    gfx::ellipsize(Face::Sans, 22, it.developer, tw), t.faint);
        }
        // Press X to open the title's page (download / install / pause live there).
        std::string action;
        switch (s.status) {
            case Status::Installed: action = "Open \xC2\xB7 installed, launch it"; break;
            case Status::Installing: action = "Open \xC2\xB7 installation in progress"; break;
            case Status::InstallIssue: action = "Open \xC2\xB7 resolve installation"; break;
            case Status::Downloaded: action = "Open \xC2\xB7 ready to install"; break;
            case Status::Partial: action = "Open \xC2\xB7 continue download"; break;
            case Status::Removing: action = "Open \xC2\xB7 removal in progress"; break;
            case Status::Queued:
            case Status::Downloading: action = "Open \xC2\xB7 downloading"; break;
            case Status::Paused: action = "Open \xC2\xB7 paused"; break;
            default: action = "Open \xC2\xB7 download (" + size_text(it.size) + ")";
        }
        const int ay = panel.bottom() - 62;
        const int gw = draw_button_glyph(c, kCross, tx, ay + 18, 34);
        gfx::draw_text(c, Face::Sans, kSmallPx, tx + gw + 14, ay + 18 - gfx::font_metrics(Face::Sans, kSmallPx).line_height / 2,
                       gfx::ellipsize(Face::Sans, kSmallPx, action, tw - gw - 14), t.muted);
    }

    int tab_;
    Category category_;
    bool installed_ = false;  // the Installed tab: installed titles of every category
    uint64_t built_revision_ = 0;
    std::vector<std::pair<Category, int>> order_;  // (list, index in it)
    GridNav nav_;
    Backdrop backdrop_;
    std::string built_search_;

public:
    // A search typed in another tab, or options changed: rebuild on the next frame.
    void check_stale() {
        if (built_search_ != g_search) rebuild();
    }
};

// "9.00" -> "9.00+" (that firmware or later); "" when not known.
std::string firmware_text(std::string fw) {
    fw = lib::trim(fw);
    if (fw.empty() || fw == "?.??") return "";
    return fw.back() == '+' ? fw : fw + "+";
}

// ---- screenshots and trailers (from the PlayStation Store) ---------------------

// A play symbol: a disc with a triangle, over a trailer's picture.
void draw_play_symbol(Canvas& c, int cx, int cy, int radius) {
    c.fill_circle(cx, cy, radius, gfx::with_alpha(gfx::rgb(0x000000), 150));
    c.stroke_circle(cx, cy, radius, std::max(2, radius / 12), gfx::rgb(0xFFFFFF));
    const int h = radius * 9 / 10, left = cx - radius * 3 / 10;
    for (int dy = -h / 2; dy <= h / 2; ++dy) {
        const int len = (h / 2 - std::abs(dy)) * 9 / 5;
        if (len > 0) c.fill({left, cy + dy, len, 1}, gfx::rgb(0xFFFFFF));
    }
}

std::string clock_text(uint64_t ms) {
    const uint64_t s = ms / 1000;
    char text[32];
    if (s >= 3600) snprintf(text, sizeof text, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(text, sizeof text, "%d:%02d", (int)(s / 60), (int)(s % 60));
    return text;
}

// A trailer, full screen. The picture shows while it loads; the controls
// show for a few seconds after a button is pressed.
class TrailerScreen : public Screen {
public:
    // Streamed: the trailer's HLS stream, from the store's video servers.
    TrailerScreen(const Item::Trailer& trailer, std::string title)
        : url_(trailer.url), poster_(trailer.poster), title_(std::move(title)) {}
    ~TrailerScreen() override { platform::movie_close(); }
    bool fullscreen() const override { return true; }
    bool own_background() const override { return true; }
    bool animating() const override { return true; }
    std::vector<Hint> hints() const override {
        if (!problem_.empty()) return {{kCircle, "Back"}};
        return {{kCircle, "Back"}, {kLeft | kRight, "Tap: 10 s, hold: scrub"}, {kCross, paused_ ? "Play" : "Pause"}};
    }

    void update(const Input& in) override {
        const uint64_t now = now_ms();
        if (!opened_) {
            opened_ = true;
            started_at_ = now;
            if (!platform::movie_open(url_, problem_)) LOG("trailer: %s", problem_.c_str());
        }
        if (in.press(kCircle)) { app().pop(); return; }
        if (!problem_.empty()) return;
        if (in.press(kCross)) {
            paused_ = !paused_;
            platform::movie_pause(paused_);
            show_controls();
        }
        scrub(in, now);
        if (in.press(kOptions) || in.press(kUp) || in.press(kDown)) show_controls();
        // The end, the stream failing once it had started, or the player torn
        // down while the app was on the home screen (even when paused).
        if (playing_ && !holding_ && !platform::movie_active()) { app().pop(); return; }
        // Failed before a picture came: say so.
        if (!playing_ && opened_ && problem_.empty() && !platform::movie_active()) {
            problem_ = platform::movie_problem();
            if (problem_.empty()) problem_ = "The trailer could not be played.";
            platform::movie_close();
        }
        if (!playing_ && now - started_at_ > 30000) {
            problem_ = "The trailer did not start. Check the network connection and try again.";
            platform::movie_close();
        }
    }

    void draw(Canvas& c) override {
        const Theme& t = theme();
        bool frame = false;
        if (problem_.empty() && opened_) frame = platform::movie_draw(c.pixels(), c.pixel_width(), c.pixel_height());
        if (frame && !playing_) { playing_ = true; show_controls(); }
        if (!frame && !playing_) {
            c.fill({0, 0, c.width(), c.height()}, gfx::rgb(0x000000));
            const gfx::Image* picture = poster_.empty() ? nullptr : gfx::image_get(store::screenshot_source(poster_), c.width(), c.height());
            if (picture) c.draw_image(*picture, (c.width() - picture->lw()) / 2, (c.height() - picture->lh()) / 2, 110);
            if (problem_.empty()) {
                draw_spinner(c, c.width() / 2, c.height() / 2, 40);
                const std::string text = "Loading trailer...";
                gfx::draw_text(c, Face::Sans, kBodyPx, (c.width() - gfx::text_width(Face::Sans, kBodyPx, text)) / 2,
                               c.height() / 2 + 70, text, t.text);
            } else {
                const int w = gfx::text_width(Face::Sans, kBodyPx, problem_);
                gfx::draw_text(c, Face::Sans, kBodyPx, std::max(kMarginX, (c.width() - w) / 2), c.height() / 2 - 20, problem_, t.text);
            }
        }
        if (!problem_.empty() || !playing_ || paused_ || holding_ || now_ms() < controls_until_) {
            c.fill_vgradient({0, 0, c.width(), 170}, gfx::with_alpha(gfx::rgb(0x000000), 200), gfx::with_alpha(gfx::rgb(0x000000), 0));
            gfx::draw_text(c, Face::SansBold, 36, kMarginX, 56, gfx::ellipsize(Face::SansBold, 36, title_, c.width() - 2 * kMarginX),
                           gfx::rgb(0xFFFFFF));
            if (playing_) draw_scrub_bar(c);
            draw_footer(c, hints());
        }
    }

private:
    std::string url_, poster_, title_, problem_;
    bool opened_ = false, playing_ = false, paused_ = false;
    uint64_t started_at_ = 0, controls_until_ = 0;
    // Left or right held: a tap skips 10 s; a hold scrubs, faster the longer
    // it is held, and the trailer jumps there on release.
    bool holding_ = false, scrubbing_ = false;
    int hold_dir_ = 0;
    uint64_t hold_since_ = 0, last_tick_ = 0;
    double target_ms_ = 0;
    // Just after a jump the player still reports the old time for a moment:
    // the bar keeps showing where it is going.
    double pending_ms_ = -1;
    uint64_t pending_until_ = 0;

    void show_controls() { controls_until_ = now_ms() + 3000; }

    double shown_position() const {
        if (holding_ && scrubbing_) return target_ms_;
        if (pending_ms_ >= 0 && now_ms() < pending_until_) return pending_ms_;
        return (double)platform::movie_position_ms();
    }

    void jump(double ms) {
        const double length = (double)platform::movie_duration_ms();
        ms = std::max(0.0, length > 0 ? std::min(ms, length - 500) : ms);
        platform::movie_seek((uint64_t)ms);
        pending_ms_ = ms;
        pending_until_ = now_ms() + 1500;
    }

    void scrub(const Input& in, uint64_t now) {
        const int dir = in.down(kRight) ? 1 : in.down(kLeft) ? -1 : 0;
        if (!playing_) return;
        if (dir != 0 && (!holding_ || dir == hold_dir_)) {
            if (!holding_) {
                holding_ = true;
                scrubbing_ = false;
                hold_dir_ = dir;
                hold_since_ = last_tick_ = now;
                target_ms_ = shown_position();
            } else if (now - hold_since_ >= 350) {
                // 8 s of trailer per second at first, rising to 40.
                scrubbing_ = true;
                const double held = (double)(now - hold_since_ - 350) / 1000.0;
                const double rate = std::min(40.0, 8.0 + held * 16.0) * 1000.0;
                const double length = (double)platform::movie_duration_ms();
                target_ms_ += hold_dir_ * rate * (double)(now - last_tick_) / 1000.0;
                target_ms_ = std::max(0.0, length > 0 ? std::min(target_ms_, length) : target_ms_);
            }
            last_tick_ = now;
            show_controls();
            return;
        }
        if (holding_) {
            if (scrubbing_) jump(target_ms_);
            else jump(shown_position() + hold_dir_ * 10000.0);
            holding_ = scrubbing_ = false;
            show_controls();
        }
    }

    void draw_scrub_bar(Canvas& c) {
        const Theme& t = theme();
        const double length = (double)platform::movie_duration_ms();
        const double at = shown_position();
        const int y = kFooterTop - 64;
        const std::string left = clock_text((uint64_t)at) + (paused_ ? "  (paused)" : "");
        const std::string right = length > 0 ? clock_text((uint64_t)length) : "";
        const int tw = std::max(gfx::text_width(Face::Sans, kSmallPx, "0:00:00"), gfx::text_width(Face::Sans, kSmallPx, left));
        const int line = gfx::font_metrics(Face::Sans, kSmallPx).line_height;
        c.fill_vgradient({0, y - 70, c.width(), c.height() - y + 70}, gfx::with_alpha(gfx::rgb(0x000000), 0),
                         gfx::with_alpha(gfx::rgb(0x000000), 170));
        gfx::draw_text(c, Face::Sans, kSmallPx, kMarginX, y - line / 2, left, gfx::rgb(0xFFFFFF));
        if (!right.empty())
            gfx::draw_text(c, Face::Sans, kSmallPx, c.width() - kMarginX - gfx::text_width(Face::Sans, kSmallPx, right), y - line / 2,
                           right, gfx::rgb(0xFFFFFF));
        const int x0 = kMarginX + tw + 32, x1 = c.width() - kMarginX - gfx::text_width(Face::Sans, kSmallPx, "0:00:00") - 32;
        const int h = scrubbing_ ? 12 : 8;
        const Rect track{x0, y - h / 2, x1 - x0, h};
        c.fill_rounded(track, h / 2, gfx::with_alpha(gfx::rgb(0xFFFFFF), 70));
        if (length > 0) {
            const double f = std::max(0.0, std::min(1.0, at / length));
            const int fill = (int)(track.w * f);
            if (fill > 0) c.fill_rounded({track.x, track.y, std::max(fill, h), h}, h / 2, t.accent);
            const int knob = scrubbing_ ? 16 : 11;
            c.fill_circle(track.x + fill, y, knob, gfx::rgb(0xFFFFFF));
            if (scrubbing_) {
                // Where release will jump to, above the knob.
                const std::string to = clock_text((uint64_t)at);
                const int w = gfx::text_width(Face::SansBold, kBodyPx, to) + 28;
                const Rect bubble{std::max(track.x, std::min(track.right() - w, track.x + fill - w / 2)), y - 82, w, 48};
                c.fill_rounded(bubble, 12, gfx::with_alpha(gfx::rgb(0x000000), 210));
                gfx::draw_text(c, Face::SansBold, kBodyPx, bubble.x + 14,
                               bubble.y + (bubble.h - gfx::font_metrics(Face::SansBold, kBodyPx).line_height) / 2, to, gfx::rgb(0xFFFFFF));
            }
        }
    }
};

// A title's trailers (first) and screenshots, one at a time, full screen.
class GalleryScreen : public Screen {
public:
    GalleryScreen(Item item, int index) : item_(std::move(item)), index_(index) {}
    bool fullscreen() const override { return true; }
    bool own_background() const override { return true; }
    bool animating() const override { return loading_; }
    std::vector<Hint> hints() const override {
        std::vector<Hint> out{{kCircle, "Back"}};
        if (count() > 1) out.push_back({kLeft | kRight, "Browse"});
        if (index_ < (int)item_.trailers.size()) out.push_back({kCross, "Play trailer"});
        return out;
    }

    void update(const Input& in) override {
        if (in.press(kCircle)) { app().pop(); return; }
        const int n = count();
        if (n == 0) return;
        if (in.repeat(kLeft) || in.repeat(kL1)) index_ = (index_ + n - 1) % n;
        if (in.repeat(kRight) || in.repeat(kR1)) index_ = (index_ + 1) % n;
        if (in.press(kCross) && index_ < (int)item_.trailers.size()) {
            const auto& trailer = item_.trailers[index_];
            app().push(std::make_unique<TrailerScreen>(trailer, item_.name));
        }
    }

    void draw(Canvas& c) override {
        const Theme& t = theme();
        c.fill({0, 0, c.width(), c.height()}, gfx::rgb(0x000000));
        const int n = count();
        const int trailers = (int)item_.trailers.size();
        const bool trailer = index_ < trailers;
        const std::string picture = n == 0 ? "" : trailer ? item_.trailers[index_].poster : item_.screenshots[index_ - trailers];
        const std::string source = store::screenshot_source(picture);
        const gfx::Image* image = source.empty() ? nullptr : gfx::image_get(source, c.width(), c.height());
        loading_ = !source.empty() && !image && gfx::image_state(source) == gfx::ImageState::Loading;
        if (image) c.draw_image(*image, (c.width() - image->lw()) / 2, (c.height() - image->lh()) / 2);
        else if (loading_) draw_spinner(c, c.width() / 2, c.height() / 2, 40);
        else if (!trailer || picture.empty()) {
            const std::string text = n == 0 ? "No screenshots for this title." : "This picture could not be loaded.";
            gfx::draw_text(c, Face::Sans, kBodyPx, (c.width() - gfx::text_width(Face::Sans, kBodyPx, text)) / 2, c.height() / 2, text, t.muted);
        }
        if (trailer) draw_play_symbol(c, c.width() / 2, c.height() / 2, 72);
        c.fill_vgradient({0, 0, c.width(), 170}, gfx::with_alpha(gfx::rgb(0x000000), 190), gfx::with_alpha(gfx::rgb(0x000000), 0));
        gfx::draw_text(c, Face::SansBold, 36, kMarginX, 56, gfx::ellipsize(Face::SansBold, 36, item_.name, c.width() - 2 * kMarginX - 420),
                       gfx::rgb(0xFFFFFF));
        if (n > 0) {
            const std::string where = trailer ? "Trailer " + std::to_string(index_ + 1) + " of " + std::to_string(trailers)
                                              : "Screenshot " + std::to_string(index_ - trailers + 1) + " of " +
                                                    std::to_string((int)item_.screenshots.size());
            gfx::draw_text(c, Face::Sans, kBodyPx, c.width() - kMarginX - gfx::text_width(Face::Sans, kBodyPx, where), 60, where,
                           gfx::rgb(0xFFFFFF));
        }
        draw_footer(c, hints());
    }

private:
    Item item_;
    int index_ = 0;
    bool loading_ = false;
    int count() const { return (int)(item_.trailers.size() + item_.screenshots.size()); }
};

// ---- the details page --------------------------------------------------------------

class DetailsScreen : public Screen {
public:
    explicit DetailsScreen(Item item) : item_(std::move(item)) {}
    ~DetailsScreen() override {
        *metadata_token_ = true;
        if (!automatic_key_.empty()) metadata_attempts().erase(automatic_key_);
    }
    bool animating() const override { return metadata_busy_ || update_busy_; }

    bool own_background() const override { return true; }
    std::vector<Hint> hints() const override { return {{kCircle, "Back"}, {kUp | kDown, "Scroll"}, {kOptions, "Options"}, {kCross, "Select"}}; }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        if (!metadata_started_) { metadata_started_ = true; auto_metadata(); }
        if (in.press(kOptions)) { open_title_options(); return; }
        const auto actions = this->actions();
        if (in.repeat(kUp)) scroll_ = std::max(0, scroll_ - 64);
        if (in.repeat(kDown)) scroll_ = std::min(max_scroll_, scroll_ + 64);
        if (in.repeat(kLeft)) focus_ = std::max(0, focus_ - 1);
        if (in.repeat(kRight)) focus_ = std::min((int)actions.size() - 1, focus_ + 1);
        focus_ = std::min(focus_, (int)actions.size() - 1);
        if (in.press(kCross) && focus_ >= 0) actions[focus_].run();
    }

    void draw(Canvas& c) override {
        const Theme& t = theme();
        const std::string art = store::cover_source(item_);
        backdrop_.draw(c, art);
        const auto jobs = store::downloads().snapshot();
        const StatusInfo s = full_status(item_, jobs);

        const int bw = 400, bh = box_height(bw);
        const Rect box{kMarginX + 30, 190, bw, bh};
        draw_game_box(c, box, art, item_.name, box_style(item_));
        draw_badge(c, box.right() - 26, box.y + 26 + bw * 135 / 1000, 24, s.status);

        const int x = box.right() + 80, w = 1920 - kMarginX - x;
        int y = 190;
        y += gfx::draw_wrapped(c, Face::SansBold, 54, x, y, w, item_.name, t.text, 2) + 18;
        int cx = x;
        const Color chip_bg = gfx::with_alpha(t.text, 30);
        cx += draw_chip(c, cx, y, store::category_name(item_.category), t.accent_text, t.accent, 24) + 12;
        cx += draw_chip(c, cx, y, item_.region, t.text, chip_bg, 24) + 12;
        const std::string v = version_text(item_);
        if (!v.empty()) cx += draw_chip(c, cx, y, v, t.text, chip_bg, 24) + 12;
        cx += draw_chip(c, cx, y, box_label(box_style(item_)), t.text, chip_bg, 24) + 12;
        if (s.status != Status::None)
            draw_chip(c, cx, y, status_label(s.status), gfx::rgb(0x0A0F1C), status_color(s.status), 24);
        y += 76;

        // Actions.
        const auto actions = this->actions();
        int bx = x;
        for (int i = 0; i < (int)actions.size(); ++i) {
            const bool on = i == focus_;
            const bool gear = actions[i].label == "Options";
            const int px = 27, bwid = gfx::text_width(Face::SansSemiBold, px, actions[i].label) + 64 + (gear ? 36 : 0);
            // A button that would pass the right edge starts the next row.
            if (bx > x && bx + bwid > x + w) {
                bx = x;
                y += 84;
            }
            const Rect r{bx, y, bwid, 68};
            if (on) {
                c.fill_rounded(r, 14, t.accent);
                c.stroke_rounded({r.x - 4, r.y - 4, r.w + 8, r.h + 8}, 17, 3, t.focus);
            } else {
                c.fill_rounded(r, 14, gfx::with_alpha(t.surface_hi, 230));
            }
            if (gear) {
                const Color color = on ? t.accent_text : t.text;
                const int gx = r.x + 36, gy = r.y + r.h / 2;
                c.stroke_circle(gx, gy, 10, 4, color);
                for (int tooth = 0; tooth < 8; ++tooth) {
                    const double angle = tooth * 3.141592653589793 / 4;
                    c.line(gx + cos(angle) * 10, gy + sin(angle) * 10, gx + cos(angle) * 15, gy + sin(angle) * 15, 4, color);
                }
            }
            gfx::draw_text(c, Face::SansSemiBold, px, r.x + 32 + (gear ? 36 : 0), r.y + (r.h - gfx::font_metrics(Face::SansSemiBold, px).line_height) / 2,
                           actions[i].label, on ? t.accent_text : t.text);
            bx += bwid + 16;
        }
        y += 100;
        if (metadata_busy_) {
            gfx::draw_text(c, Face::Sans, kSmallPx, x, y, "Looking up game information...", t.muted);
            y += 42;
        }
        const std::string install_path = item_install_path(item_, jobs);
        const auto install = store::installation_progress(install_path);
        const bool install_held = store::installation_uses_file(install_path);
        if (install_held && install.attention != store::InstallAttention::None) {
            // Stalled or failed with the task still held: why, and where to go.
            y += gfx::draw_wrapped(c, Face::Sans, kSmallPx, x, y, w, install.message + " Select Resolve installation.",
                                   t.warning, 5) + 14;
        } else if (s.status != Status::Installed) for (const auto& job : jobs) {
            if (job.item.url != item_.url) continue;
            std::string note;
            Color color = t.warning;
            if (job.state == store::JobState::Done && job.install_state == store::DownloadInstallState::Failed)
                note = "Downloaded successfully. Installation failed. The PKG is kept; select Install to retry.";
            else if (job.state == store::JobState::Done && job.install_state == store::DownloadInstallState::Unverified)
                note = "Downloaded successfully. The installation could not be verified, so it is not marked as installed. "
                       "The PKG is kept; select Install to retry.";
            else if (job.state == store::JobState::Failed) {
                note = "Download failed: " + job.message;
                color = t.error;
            } else if (job.state == store::JobState::Paused && !job.message.empty() && job.message != "Paused.")
                note = job.message;
            if (note.empty()) continue;
            y += gfx::draw_wrapped(c, Face::Sans, kSmallPx, x, y, w, note, color, 3) + 14;
            break;
        }

        if (install_held && install.attention != store::InstallAttention::None) {
            // No bar: nothing is progressing.
        } else if (install_held && install.state != store::InstallTaskState::Failed) {
            // Only while the install runs: once it is done the bar goes, and
            // the Installed badge and Launch say the rest.
            draw_progress(c, {x, y, std::min(w, 900), 14}, std::max(0, install.percent) / 100.f);
            gfx::draw_text(c, Face::Sans, kSmallPx, x, y + 26, install_progress_text(install), t.muted);
            y += 80;
        }

        // A running download's progress.
        for (const auto& j : jobs) {
            if (j.item.url != item_.url || j.state != store::JobState::Downloading) continue;
            draw_progress(c, {x, y, std::min(w, 900), 14}, j.total ? (float)((double)j.done / j.total) : 0.f);
            std::string line = lib::format_bytes(j.done) + " of " + lib::format_bytes(j.total);
            if (j.speed > 0) line += "  \xC2\xB7  " + lib::format_bytes((uint64_t)j.speed) + "/s  \xC2\xB7  " +
                                     duration_text((double)(j.total - j.done) / j.speed);
            gfx::draw_text(c, Face::Sans, kSmallPx, x, y + 26, line, t.muted);
            y += 80;
        }

        // Keep actions visible while the overview and facts scroll below them.
        const int facts_top = y;
        c.push_clip({x, facts_top, w, kFooterTop - facts_top - 16});
        y -= scroll_;
        // The overview, paragraph by paragraph (the store's has several).
        if (item_.description.empty()) {
            y += gfx::draw_wrapped(c, Face::Sans, kBodyPx, x, y, w, "No overview supplied for this title.", t.muted, 2) + 24;
        } else {
            int lines_left = 5;
            // (0 only without fonts; never divided by.)
            const int line_h = std::max(1, gfx::font_metrics(Face::Sans, kBodyPx).line_height);
            for (const std::string& paragraph : lib::split(item_.description, '\n')) {
                if (lines_left <= 0) break;
                if (lib::trim(paragraph).empty()) { y += line_h / 2; continue; }
                const int used = gfx::draw_wrapped(c, Face::Sans, kBodyPx, x, y, w, paragraph, t.muted, lines_left);
                lines_left -= used / line_h;
                y += used;
            }
            y += 24;
        }
        // Facts: only what is known.
        struct Row {
            const char* label;
            std::string value;
        };
        const std::string& src = item_.metadata_source;
        const std::string information = lib::starts_with(src, "https://store.playstation.com/") ? "PlayStation Store" :
            lib::starts_with(src, "https://github.com/libretro-thumbnails/") ? "Screenshots: libretro-thumbnails" : "";
        std::vector<Row> rows = {{"Title ID", item_.title_id},
                                 {"Size", item_.size ? size_text(item_.size) : ""},
                                 {"Version", item_.version},
                                 {"Required firmware", firmware_text(item_.min_fw)},
                                 {"Release date", item_.release_date},
                                 {"Publisher", item_.publisher},
                                 {"Developer", item_.developer},
                                 {"Genre", item_.genres},
                                 {"Players", item_.players},
                                 {"Languages", item_.languages},
                                 {"Age rating", item_.rating},
                                 {"Information", information},
                                 {"Package", store::package_path(item_)},
                                 {"Source", host_of(item_.url)}};
        if (item_.category == Category::Homebrew) {
            rows = {{"Category", item_.homebrew_category},
                    {"Credits", item_.developer.empty() ? "Not supplied" : item_.developer},
                    {"Version", item_.version.empty() ? "Unknown" : item_.version},
                    {"Latest release", item_.latest_release_date.empty() ? "Unknown" : item_.latest_release_date},
                    {"Required firmware", firmware_text(item_.min_fw).empty() ? "Unknown" : firmware_text(item_.min_fw)},
                    {"Title ID", item_.title_id},
                    {"Size", size_text(item_.size)},
                    {"Project", item_.metadata_source.empty() ? host_of(item_.url) : item_.metadata_source},
                    {"Package", store::package_path(item_)}};
        }
        for (const Row& r : rows) {
            if (r.value.empty()) continue;
            gfx::draw_text(c, Face::Sans, kBodyPx, x, y, r.label, t.faint);
            gfx::draw_text(c, Face::Sans, kBodyPx, x + 290, y,
                           gfx::ellipsize(Face::Sans, kBodyPx, r.value, w - 290), t.text);
            y += 46;
        }
        // Trailers and screenshots, under the facts: the first trailer, then screenshots
        // (more trailers when there are few). The buttons above show them all.
        struct Thumb { std::string picture; bool trailer; };
        std::vector<Thumb> thumbs;
        if (!item_.trailers.empty()) thumbs.push_back({item_.trailers[0].poster, true});
        for (const auto& shot : item_.screenshots) if (thumbs.size() < 6) thumbs.push_back({shot, false});
        for (size_t i = 1; i < item_.trailers.size() && thumbs.size() < 6; ++i) thumbs.push_back({item_.trailers[i].poster, true});
        if (!thumbs.empty()) {
            y += 20;
            const int gap = 16, tw = (w - 2 * gap) / 3, th = tw * 9 / 16;
            for (size_t i = 0; i < thumbs.size(); ++i) {
                const Rect r{x + (int)(i % 3) * (tw + gap), y + (int)(i / 3) * (th + gap), tw, th};
                c.fill_rounded(r, 12, gfx::with_alpha(t.surface_hi, 200));
                const gfx::Image* image = thumbs[i].picture.empty() ? nullptr :
                    gfx::image_get(store::screenshot_source(thumbs[i].picture), tw, th, gfx::Fit::Cover);
                if (image) c.draw_image_rounded(*image, r.x, r.y, 12);
                if (thumbs[i].trailer) draw_play_symbol(c, r.x + tw / 2, r.y + th / 2, 34);
            }
            y += (int)((thumbs.size() + 2) / 3) * (th + gap) + 20;
        }
        c.pop_clip();
        max_scroll_ = std::max(0, y + scroll_ - kFooterTop + 32);
        scroll_ = std::min(scroll_, max_scroll_);
    }

private:
    int scroll_ = 0, max_scroll_ = 0;
    lib::CancelToken metadata_token_ = lib::make_token();
    bool metadata_busy_ = false;
    bool metadata_started_ = false;
    std::string automatic_key_;
    bool update_busy_ = false;

    static std::set<std::string>& metadata_attempts() {
        static std::set<std::string> attempted;
        return attempted;
    }

    static void message(const std::string& title, const std::string& text) {
        open_menu(title, {{"OK", nullptr, [] {}, nullptr}}, text);
    }

    void open_title_options() {
        const Item item = item_;
        std::vector<MenuItem> options;
        if (item.category != Category::Homebrew)
            options.push_back({"Find game info", nullptr, [this] { find_metadata(); }, nullptr});
        if (store::app_delete_problem(item).empty()) {
            MenuItem remove{item.category == Category::Games ? "Delete installed game" : "Delete installed app", nullptr, [item] {
                confirm("Delete " + item.name + "?", "Remove installed title " + item.title_id +
                    ". Downloaded PKG files are kept.", "Delete app", [item] {
                        const auto result = store::delete_app(item);
                        forget_file_states(); app().toast(result.message);
                    });
            }, nullptr};
            remove.destructive = true; options.push_back(remove);
        }
        std::set<std::string> paths;
        const auto has_download = [](const std::string& path) {
            return lib::file_exists(path) || lib::file_exists(path + ".part") || lib::file_exists(path + ".resume");
        };
        std::vector<std::string> roots{store::console_download_path()};
        for (const auto& drive : store::connected_usb_drives()) roots.push_back(drive);
        for (const auto& root : roots) {
            const std::string path = lib::join_path(root, store::package_file_name(item));
            if (has_download(path)) paths.insert(path);
        }
        for (const auto& job : store::downloads().snapshot())
            if (job.item.title_id == item.title_id && has_download(job.path)) paths.insert(job.path);
        for (const auto& path : paths) {
            const std::string label = std::string(lib::file_exists(path) ? "Delete downloaded PKG" : "Delete partial download") +
                (paths.size() > 1 ? " (" + download_location_name(path) + ")" : "");
            MenuItem remove{label, [path] { return lib::format_bytes(lib::file_size(path)); }, [item, path] {
                confirm("Delete download files?", item.name + "\n" + path +
                    "\nPartial files and resume data are also deleted. The installed game or app is kept.", "Delete download", [path] {
                        std::string problem;
                        const bool deleted = store::delete_downloaded_package(path, problem);
                        forget_file_states(); app().toast(deleted ? "Download files deleted." : problem);
                    });
            }, nullptr};
            remove.destructive = true; options.push_back(remove);
        }
        if (options.empty()) options.push_back({"Close", nullptr, [] {}, nullptr});
        open_menu("Options", std::move(options), item.name);
    }

    void check_updates() {
        if (update_busy_) return;
        update_busy_ = true;
        const Item item = item_;
        const std::string downloaded_path = store::package_path(item);
        app().toast("Checking for updates to " + item.name + "...");
        lib::async<store::HomebrewUpdate>(metadata_token_, [item, downloaded_path] { return store::check_homebrew_update(item, downloaded_path); },
            [this](store::HomebrewUpdate& result) {
                update_busy_ = false;
                if (!store::is_installed(item_)) { message("Update check", "This app is no longer installed."); return; }
                if (!result.problem.empty()) { message("Could not check for updates", result.problem); return; }
                if (!result.available) {
                    const int comparison = store::compare_app_versions(result.installed_version, result.latest_version);
                    message(comparison == 1 ? "No newer release available" : "Already on the latest version",
                        item_.name + "\nInstalled: " + result.installed_version + ". Latest published: " + result.latest_version + ".");
                    return;
                }
                const Item update = result.item;
                if (result.downloaded) {
                    const std::string path = result.downloaded_path;
                    confirm("Update already downloaded",
                        "Version " + result.latest_version + " is already saved at " + path + ".",
                        store::installer_ready() ? "Install update" : "How to install",
                        [update, path] { app().toast(store::install_local(path, update).message); });
                    return;
                }
                confirm("Update " + update.name + "?",
                    "Installed: " + result.installed_version + ". Latest: " + result.latest_version +
                    ". Download the newer package to your selected destination?",
                    "Download update", [update] { get_item_direct(update); });
            });
    }

    void accept_metadata(store::MetadataResult& found, bool automatic) {
        metadata_busy_ = false;
        if (!found.problem.empty()) {
            if (!automatic) app().toast(found.problem);
            return;
        }
        if (found.fields["metadata_source"].str().empty()) return;
        if (!store::save_game_metadata(item_, found.fields)) {
            if (!automatic) app().toast("Could not save game information.");
            return;
        }
        store::apply_game_metadata(item_, found.fields);
        scroll_ = 0;
        store::catalog()->update_game_metadata(item_.title_id, found.fields);
        if (!automatic) app().toast("Game information saved from the PlayStation Store.");
    }

    void auto_metadata() {
        if (!store::needs_game_metadata(item_)) return;
        // Once per title/name per run, including misses. Reopening a details
        // page should not repeatedly call the service or show selection menus.
        const std::string key = item_.title_id + ":" + item_.name;
        if (!metadata_attempts().insert(key).second) return;
        automatic_key_ = key;
        metadata_busy_ = true;
        const Item item = item_;
        lib::async<store::MetadataResult>(metadata_token_, [item] { return store::auto_game_metadata(item); },
            [this, key](store::MetadataResult& found) {
                automatic_key_.clear();
                // A miss for now (offline, the store busy): asked again next visit.
                if (found.retry_later) metadata_attempts().erase(key);
                accept_metadata(found, true);
            });
    }

    void find_metadata() {
        if (metadata_busy_) return;
        open_keyboard("Find this game on the PlayStation Store", store::game_metadata_query(item_.name), KeyboardKind::Text,
            [this](const std::string& query) {
                if (lib::trim(query).empty()) return;
                metadata_busy_ = true;
                const Item item = item_;
                lib::async<store::MetadataSearch>(metadata_token_, [item, query] { return store::search_game_metadata(item, query); },
                    [this](store::MetadataSearch& result) {
                        metadata_busy_ = false;
                        if (!result.problem.empty()) { app().toast(result.problem); return; }
                        std::vector<MenuItem> matches;
                        for (const auto& match : result.matches) {
                            matches.push_back({match.name + " - " + match.description, nullptr, [this, match] {
                                metadata_busy_ = true;
                                const auto item = item_;
                                lib::async<store::MetadataResult>(metadata_token_, [item, match] {
                                    return store::fetch_game_metadata(item, match.id);
                                }, [this](store::MetadataResult& found) {
                                    accept_metadata(found, false);
                                });
                            }, nullptr});
                        }
                        open_menu("Choose the matching game", std::move(matches), "Check the name, console and title ID before selecting.");
                    });
            });
    }
    struct Action {
        std::string label;
        std::function<void()> run;
    };

    static std::string host_of(const std::string& url) {
        const size_t start = url.find("://");
        if (start == std::string::npos) return url;
        const size_t end = url.find('/', start + 3);
        return url.substr(start + 3, end == std::string::npos ? std::string::npos : end - start - 3);
    }

    std::vector<Action> actions() {
        std::vector<Action> out;
        const Item item = item_;
        const auto jobs = store::downloads().snapshot();
        const StatusInfo s = full_status(item, jobs);
        if (s.status == Status::Removing) {
            out.push_back({"Removing...", [] {}});
            out.push_back({"Options", [this] { open_title_options(); }});
            return out;
        }
        const std::string install_path = item_install_path(item, jobs);
        if (store::installation_uses_file(install_path)) {
            const auto attention = store::installation_progress(install_path).attention;
            if (attention == store::InstallAttention::None) {
                out.push_back({"Installing...", [] {}});
                out.push_back({"Options", [this] { open_title_options(); }});
                return out;
            }
            out.push_back({"Resolve installation", [install_path, item] { open_install_recovery(install_path, item.name); }});
            // Installed and verified, only its task is held: the title's
            // usual actions follow. Otherwise nothing else applies yet.
            if (attention != store::InstallAttention::ReleaseStalled) {
                out.push_back({"Options", [this] { open_title_options(); }});
                return out;
            }
        }
        if (s.status == Status::Downloading || s.status == Status::Queued) {
            out.push_back({"Pause", [item] {
                               store::downloads().pause(item.url);
                               app().toast("Paused " + item.name);
                           }});
            out.push_back({"Stop", [item] {
                               store::downloads().cancel(item.url);
                               app().toast("Stopped. The file is kept to continue later.");
                           }});
        } else if (s.status == Status::Paused) {
            out.push_back({"Resume", [item] {
                               std::string problem;
                               app().toast(store::downloads().resume(item.url, &problem) ? "Resuming " + item.name : problem);
                           }});
            out.push_back({"Stop", [item] {
                               store::downloads().cancel(item.url);
                               app().toast("Stopped. The file is kept to continue later.");
                           }});
        } else if (s.status == Status::Downloaded) {
            out.push_back({store::installer_ready() ? "Install" : "How to install", [item] { app().toast(store::install_local(store::package_path(item), item).message); }});

        } else {
            const bool partial = s.status == Status::Partial;
            // An installed game or app: the button that was Download starts it.
            // (Updates, add-ons and themes are not things to start.)
            const bool startable = s.status == Status::Installed && item.category != Category::Updates &&
                                   item.category != Category::DLC && item.category != Category::Themes;
            if (startable)
                out.push_back({"Launch", [item] {
                                   const auto start = [item] { app().toast(store::launch_title(item).message); };
                                   if (store::downloads().active() > 0 && !store::downloads().using_service())
                                       confirm("Launch " + item.name + "?",
                                               "The console closes FPKGi+ to run it. The queue is saved: the running "
                                               "download comes back paused, ready to resume, the next time FPKGi+ is opened.",
                                               "Launch", start);
                                   else start();
                               }});
            if (s.status == Status::Installed) {
                if (item.category == Category::Homebrew)
                    out.push_back({update_busy_ ? "Checking..." : "Check for updates", [this] { check_updates(); }});
            } else {
                out.push_back({partial ? "Continue download" : "Download", [item] { get_item_direct(item); }});
            }

        }
        if (!item.trailers.empty())
            out.push_back({"Watch trailer", [item] {
                               app().push(std::make_unique<TrailerScreen>(item.trailers[0], item.name));
                           }});
        if (!item.screenshots.empty())
            out.push_back({"Screenshots (" + std::to_string(item.screenshots.size()) + ")", [item] {
                               app().push(std::make_unique<GalleryScreen>(item, (int)item.trailers.size()));
                           }});
        out.push_back({"Options", [this] { open_title_options(); }});
        // The game's own updates and add-ons, from the Updates and DLC lists
        // (packages made for this title: Sony's official updates do not install
        // over a package like these, so the console's own update offer fails).
        if (item.category != Category::Updates && item.category != Category::DLC) {
            const std::vector<Item> extras = related(item);
            if (!extras.empty())
                out.push_back({"Updates & add-ons (" + std::to_string(extras.size()) + ")", [item, extras] {
                                   std::vector<MenuItem> rows;
                                   for (const Item& extra : extras) {
                                       const bool update = extra.category == Category::Updates;
                                       std::string label = update ? "Update " + version_text(extra) : extra.name;
                                       if (update && version_text(extra).empty()) label = extra.name;
                                       rows.push_back({label,
                                                       [extra] {
                                                           const StatusInfo st = full_status(extra, store::downloads().snapshot());
                                                           return st.status == Status::None ? size_text(extra.size)
                                                                                           : std::string(status_label(st.status));
                                                       },
                                                       [extra] {
                                                           if (store::Downloads::file_state(extra) == store::Downloads::FileState::Complete)
                                                               app().toast(store::install_local(store::package_path(extra), extra).message);
                                                           else get_item_direct(extra);
                                                       },
                                                       nullptr, true});
                                   }
                                   open_menu("Updates & add-ons", std::move(rows),
                                             "For " + item.name + ". Install the game first, then its update, then add-ons. "
                                             "Official updates from Sony cannot be installed over this kind of package.");
                               }});
        }
        return out;
    }

    // Updates (newest first) and add-ons in the content lists made for this title.
    static std::vector<Item> related(const Item& item) {
        std::vector<Item> out;
        const auto cat = store::catalog();
        if (!cat->loaded() || item.title_id.empty()) return out;
        for (Category c : {Category::Updates, Category::DLC})
            for (const Item& it : cat->items(c))
                if (it.title_id == item.title_id && it.url != item.url) out.push_back(it);
        std::stable_sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
            if (a.category != b.category) return a.category == Category::Updates;
            if (a.category == Category::Updates) return lib::natural_less(b.version, a.version);
            return lib::natural_less(lib::lower(a.name), lib::lower(b.name));
        });
        return out;
    }

    Item item_;
    int focus_ = 0;
    Backdrop backdrop_;
};

// ---- the Downloads tab ----------------------------------------------------------------

struct Row {
    Item item;
    bool from_job = false;
    store::Job job;
    store::Downloads::FileState file = store::Downloads::FileState::None;
    std::string path;  // a file in the download folder
    uint64_t size = 0;  // that file's size, read when the folder is scanned
};

class DownloadsScreen : public Screen {
public:
    explicit DownloadsScreen(int tab) : tab_(tab) { scan(); }

    int tab() const override { return tab_; }
    bool animating() const override { return store::downloads().active() > 0; }
    void resumed() override { sizes_.clear(); scan(); }

    std::vector<Hint> hints() const override {
        std::vector<Hint> h = {{kCircle, "Back"}, {kTouchPad, "Top"}, {kTriangle, "Clear finished"}};
        if (rows_.empty()) return h;
        const Row r = rows_[nav_.index];
        const bool running = r.from_job &&
                             (r.job.state == store::JobState::Queued || r.job.state == store::JobState::Downloading);
        const bool paused = r.from_job && r.job.state == store::JobState::Paused;
        if (running || paused) h.push_back({kCross, paused ? "Resume" : "Pause"});
        const std::string package = r.from_job ? r.job.path : r.path;
        const bool installing = store::installation_uses_file(package);
        const auto install = store::installation_progress(package);
        if (running || paused) h.push_back({kSquare, "Stop"});
        else if (!r.path.empty() && !installing) h.push_back({kSquare, "Delete file"});
        if (installing && install.attention != store::InstallAttention::None) h.push_back({kCross, "Resolve"});
        if (running || paused || installing) return h;
        const bool installed = install.state == store::InstallTaskState::Complete ||
                               (r.from_job && r.job.install_state == store::DownloadInstallState::Installed);
        if (r.file == store::Downloads::FileState::Complete)
            h.push_back({kCross, !store::installer_ready() ? "How to install" : installed ? "Reinstall" : "Install"});
        else if (r.file == store::Downloads::FileState::Partial && !r.item.url.empty()) h.push_back({kCross, "Continue"});
        else if (r.from_job && (r.job.state == store::JobState::Failed || r.job.state == store::JobState::Cancelled) &&
                 !r.item.url.empty()) h.push_back({kCross, "Retry"});
        return h;
    }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().switch_tab(0);
            return;
        }
        if (in.press(kTriangle)) {
            std::vector<std::string> files;
            for (const auto& row : rows_)
                if (!row.from_job && !row.path.empty()) files.push_back(row.path);
            store::downloads().clear_finished(files);
            scan();
            return;
        }
        // The list changes as downloads finish.
        if (platform::now_ms() - scanned_at_ > 1500 || scanned_revision_ != store::downloads().revision()) scan();
        if (in.press(kTouchPad)) { nav_.index = nav_.top = 0; return; }
        if (rows_.empty()) return;
        nav_.update(in, (int)rows_.size(), kVisible, false);
        const Row r = rows_[nav_.index];
        const bool running = r.from_job &&
                             (r.job.state == store::JobState::Queued || r.job.state == store::JobState::Downloading);
        const bool paused = r.from_job && r.job.state == store::JobState::Paused;
        if (in.press(kSquare)) {
            if (running || paused) {
                store::downloads().cancel(r.item.url);
            } else if (!r.path.empty()) {
                const std::string path = r.path;
                confirm("Delete this file?", path, "Delete", [this, path] {
                    std::string problem;
                    if (!store::delete_downloaded_package(path, problem)) { app().toast(problem); return; }
                    sizes_.clear();
                    forget_file_states();
                    app().toast("Download files deleted.");
                    scan();
                });
            }
        }
        if (in.press(kCross)) {
            const std::string package = r.from_job ? r.job.path : r.path;
            std::string problem;
            if (paused || running) {
                if (!paused) store::downloads().pause(r.item.url);
                else if (!store::downloads().resume(r.item.url, &problem)) app().toast(problem);
                scan();
            } else if (store::installation_uses_file(package)) {
                // Nothing to press while it installs; a stalled one has its ways out.
                open_install_recovery(package, r.item.name);
            } else if (r.file == store::Downloads::FileState::Complete) {
                app().toast(store::install_local(r.path, r.item).message);
            } else if (!r.item.url.empty() && (r.file == store::Downloads::FileState::Partial ||
                       (r.from_job && (r.job.state == store::JobState::Failed || r.job.state == store::JobState::Cancelled)))) {
                if (r.from_job && r.job.state != store::JobState::Done) {
                    if (!store::downloads().resume(r.item.url, &problem)) app().toast(problem);
                } else if (!store::downloads().add(r.item)) {
                    app().toast("Could not queue " + r.item.name + ". Check its download destination.");
                }
                scan();
            }
        }
    }

    void draw(Canvas& c) override {
        const Theme& t = theme();
        const int tw = gfx::draw_text(c, Face::SansBold, 40, kMarginX, 150, "Downloads", t.text);
        const std::string where = std::string("Saving to: ") + (store::catalog()->config().download_to_usb ? "USB drive" : "Console") +
                                  (space_text_.empty() ? "" : "  \xC2\xB7  " + space_text_);
        gfx::draw_text(c, Face::Sans, kSmallPx, kMarginX + tw + 24, 164,
                       gfx::ellipsize(Face::Sans, kSmallPx, where, kContentW - tw - 40), t.faint);
        if (rows_.empty()) {
            draw_message(c, {0, 240, c.width(), 700}, "Nothing downloaded yet",
                         "Open a title and choose Download. Downloads can be paused, resumed and stopped, and they "
                         "continue where they stopped. FPKGi's own downloads in this folder appear here too.");
            return;
        }
        const auto jobs = store::downloads().snapshot();
        // Queue positions: how many are waiting ahead of each queued download.
        std::map<uint64_t, int> queue_pos;
        int waiting = 0;
        for (const auto& j : jobs)
            if (j.state == store::JobState::Queued) queue_pos[j.id] = ++waiting;
        int y = 230;
        for (int i = nav_.top; i < std::min((int)rows_.size(), nav_.top + kVisible); ++i) {
            Row r = rows_[i];
            if (r.from_job)
                for (const auto& j : jobs)
                    if (j.id == r.job.id) r.job = j;
            const bool on = i == nav_.index;
            const Rect row{kMarginX, y, kContentW - 30, kRowH - 16};
            c.fill_rounded(row, 16, on ? t.surface_hi : gfx::with_alpha(t.surface, 200));
            if (on) c.stroke_rounded({row.x - 4, row.y - 4, row.w + 8, row.h + 8}, 19, 3, t.focus);
            const int bh = row.h - 20, bw = bh * 1000 / 1255;
            draw_game_box(c, {row.x + 24, row.y + 10, bw, bh}, store::cover_source(r.item), r.item.name,
                          box_style(r.item), false);
            const int tx = row.x + 24 + bw + 32, tw = row.w - (tx - row.x) - 40;
            // Title and its ID on one line; the status under them; the bar on its own at the bottom.
            const int idw = gfx::text_width(Face::Sans, kSmallPx, r.item.title_id);
            gfx::draw_text(c, Face::SansBold, 30, tx, row.y + 20,
                           gfx::ellipsize(Face::SansBold, 30, r.item.name, tw - idw - 30), t.text);
            gfx::draw_text(c, Face::Sans, kSmallPx, tx + tw - idw, row.y + 28, r.item.title_id, t.faint);
            std::string state;
            Color state_color = t.muted;
            float progress = -1;
            if (r.from_job) {
                const auto& j = r.job;
                switch (j.state) {
                    case store::JobState::Queued: {
                        auto p = queue_pos.find(j.id);
                        state = p != queue_pos.end() && p->second > 1 ? "Waiting \xC2\xB7 #" + std::to_string(p->second)
                                                                      : "Waiting \xC2\xB7 next";
                        break;
                    }
                    case store::JobState::Downloading:
                        progress = j.total ? (float)((double)j.done / j.total) : 0.f;
                        state = (j.total ? std::to_string((int)(progress * 100)) + "%  \xC2\xB7  " : std::string()) +
                                lib::format_bytes(j.done) + " of " + lib::format_bytes(j.total);
                        if (j.speed > 0)
                            state += "  \xC2\xB7  " + lib::format_bytes((uint64_t)j.speed) + "/s  \xC2\xB7  " +
                                     duration_text((double)(j.total - j.done) / j.speed);
                        state_color = t.accent;
                        break;
                    case store::JobState::Paused:
                        progress = j.total ? (float)((double)j.done / j.total) : 0.f;
                        state = "Paused  \xC2\xB7  " + lib::format_bytes(j.done) +
                                (j.total ? " of " + lib::format_bytes(j.total) : "");
                        state_color = t.muted;
                        // Paused for a reason (its drive is missing, the app closed mid-download).
                        if (!j.message.empty() && j.message != "Paused.") {
                            state += "  \xC2\xB7  " + j.message;
                            state_color = t.warning;
                        }
                        break;
                    case store::JobState::Done: {
                        // The download is done either way; this is about the install.
                        const auto install = store::installation_progress(j.path);
                        const bool held = store::installation_uses_file(j.path);
                        state_color = t.success;
                        if (held && install.attention != store::InstallAttention::None) {
                            state = install_progress_text(install);
                            state_color = t.warning;
                        } else if (held && install.state != store::InstallTaskState::Failed) {
                            state = install_progress_text(install);  // "Installing 42% ..."
                            state_color = t.accent;
                            progress = std::max(0, install.percent) / 100.f;
                        } else if (j.install_state == store::DownloadInstallState::Failed || held) {
                            state = "Downloaded - installation failed; PKG kept";
                            state_color = t.warning;
                        } else if (j.install_state == store::DownloadInstallState::Unverified) {
                            state = "Downloaded - installation not verified; PKG kept";
                            state_color = t.warning;
                        } else if (j.install_state == store::DownloadInstallState::Installed) {
                            state = "Installed";  // finished: no bar
                        } else if (j.install_state == store::DownloadInstallState::Installing) {
                            state = "Installing";
                            state_color = t.accent;
                        } else {
                            state = store::installer_ready() ? "Downloaded" : "Downloaded - install from Debug settings > Package installer";
                        }
                        break;
                    }
                    case store::JobState::Failed: state = "Download failed: " + j.message; state_color = t.error; break;
                    case store::JobState::Cancelled: state = j.message; break;
                }
            } else if (r.file == store::Downloads::FileState::Complete) {
                state = (store::installer_ready() ? "Ready to install  \xC2\xB7  " : "Downloaded  \xC2\xB7  ") + lib::format_bytes(r.size);
                state_color = t.warning;
                const auto install = store::installation_progress(r.path);
                const bool held = store::installation_uses_file(r.path);
                if (held && install.attention != store::InstallAttention::None) state = install_progress_text(install);
                else if (install.state == store::InstallTaskState::Failed) state = "Downloaded - installation failed; PKG kept";
                else if (install.state == store::InstallTaskState::Complete || held) {
                    state = install_progress_text(install);
                    // A bar only while the install runs, not once it is done.
                    if (held) progress = std::max(0, install.percent) / 100.f;
                    state_color = held ? t.accent : t.success;
                } else if (!install.message.empty()) {
                    // An install that ended without being confirmed (this session).
                    state = "Downloaded - installation not verified; PKG kept";
                }
            } else {
                state = "Partly downloaded  \xC2\xB7  " + lib::format_bytes(r.size);
                if (r.item.url.empty()) state += " (not in your content lists, cannot continue)";
                state_color = gfx::rgb(0xE59A5C);
            }
            state = download_location_name(r.from_job ? r.job.path : r.path) + "  \xC2\xB7  " + state;
            gfx::draw_text(c, Face::SansSemiBold, kSmallPx, tx, row.y + 68, gfx::ellipsize(Face::SansSemiBold, kSmallPx, state, tw),
                           state_color);
            if (progress >= 0) draw_progress(c, {tx, row.bottom() - 24, tw, 10}, progress);
            y += kRowH;
        }
        draw_scrollbar(c, {1920 - kMarginX + 6, 230, 6, kVisible * kRowH - 16}, nav_.top, kVisible, (int)rows_.size());
    }

private:
    static constexpr int kVisible = 5;
    static constexpr int kRowH = 150;

    static std::string row_key(const Row& row) {
        std::string path = row.from_job ? row.job.path : row.path;
        for (const char* suffix : {".part", ".resume"})
            if (lib::ends_with(path, suffix)) { path.resize(path.size() - strlen(suffix)); break; }
        return path;
    }

    void scan() {
        scanned_at_ = platform::now_ms();
        const uint64_t revision = store::downloads().revision();
        forget_file_states();
        const int old_index = nav_.index, old_top = nav_.top;
        const std::string keep = rows_.empty() ? "" : row_key(rows_[std::clamp(nav_.index, 0, (int)rows_.size() - 1)]);
        rows_.clear();
        std::set<std::string> paths;
        for (const auto& j : store::downloads().snapshot()) {
            Row r;
            r.item = j.item;
            r.from_job = true;
            r.job = j;
            if (j.state == store::JobState::Done) {
                if (!lib::file_exists(j.path)) continue;
                r.file = store::Downloads::file_state(j.item, j.path);
                r.path = j.path;
            } else if (j.state == store::JobState::Cancelled || j.state == store::JobState::Failed || j.state == store::JobState::Paused) {
                if (lib::file_exists(j.path)) {
                    r.file = store::Downloads::file_state(j.item, j.path); r.path = j.path;
                } else if (lib::file_exists(j.path + ".part")) {
                    r.file = store::Downloads::FileState::Partial; r.path = j.path + ".part";
                } else if (lib::file_exists(j.path + ".resume")) {
                    r.file = store::Downloads::FileState::Partial; r.path = j.path + ".resume";
                }
            }
            paths.insert(j.path);
            rows_.push_back(std::move(r));
        }
        // Files in the download folder (FPKGi's too), matched to the catalogue by name.
        const std::string dir = store::catalog()->config().download_path;
        // "Not reported" is not "full": a drive reached through FTP cannot be asked.
        uint64_t free_bytes = 0;
        space_text_ = dir.empty() ? "" : lib::free_space(dir, free_bytes) ? lib::format_bytes(free_bytes) + " free"
                                                                           : "free space not reported";
        std::vector<lib::DirEntry> entries;
        lib::list_dir(dir, entries, false);
        std::map<std::string, const Item*> by_path;
        const auto cat = store::catalog();
        for (int c = 0; c < (int)Category::Count; ++c)
            for (const auto& it : cat->items((Category)c)) by_path[store::package_path(it)] = &it;
        std::sort(entries.begin(), entries.end(), [](const lib::DirEntry& a, const lib::DirEntry& b) {
            return lib::natural_less(lib::lower(a.name), lib::lower(b.name));
        });
        for (const auto& e : entries) {
            if (e.is_dir) continue;
            std::string pkg = lib::join_path(dir, e.name);
            store::Downloads::FileState state;
            if (lib::ends_with(e.name, ".pkg")) state = store::Downloads::FileState::Complete;
            else if (lib::ends_with(e.name, ".pkg.part") || lib::ends_with(e.name, ".pkg.resume")) {
                state = store::Downloads::FileState::Partial;
                pkg = pkg.substr(0, pkg.rfind(".pkg") + 4);
            } else {
                continue;
            }
            if (paths.count(pkg)) continue;
            // A partial file and its finished one: show the finished one only.
            if (state == store::Downloads::FileState::Partial && lib::file_exists(pkg)) continue;
            paths.insert(pkg);
            Row r;
            r.file = state;
            r.path = lib::join_path(dir, e.name);
            // Read once: on the console every size costs opening the file.
            auto known = sizes_.find(r.path);
            if (known == sizes_.end()) known = sizes_.emplace(r.path, lib::file_size(r.path)).first;
            r.size = known->second;
            if (store::downloads().file_cleared(r.path, r.size)) continue;
            auto found = by_path.find(pkg);
            if (found != by_path.end()) {
                r.item = *found->second;
                if (state == store::Downloads::FileState::Complete) r.file = store::Downloads::file_state(r.item, pkg);
            } else {
                // "[CUSA00001] Name.pkg" with no entry in the content lists.
                std::string name = e.name.substr(0, e.name.find(".pkg"));
                if (name.size() > 12 && name[0] == '[' && name.find(']') != std::string::npos) {
                    r.item.title_id = name.substr(1, name.find(']') - 1);
                    name = lib::trim(name.substr(name.find(']') + 1));
                }
                r.item.name = name;
            }
            rows_.push_back(std::move(r));
        }
        nav_.index = old_index;
        for (int i = 0; i < (int)rows_.size(); ++i)
            if (!keep.empty() && row_key(rows_[i]) == keep) { nav_.index = i; break; }
        nav_.top = std::max(0, old_top + nav_.index - old_index);
        nav_.clamp((int)rows_.size(), kVisible);
        scanned_revision_ = revision;
    }

    int tab_;
    std::vector<Row> rows_;
    ListNav nav_;
    uint64_t scanned_at_ = 0;
    uint64_t scanned_revision_ = 0;
    std::map<std::string, uint64_t> sizes_;  // file sizes already read
    std::string space_text_;  // "12.3 GB free", for the heading
};

}  // namespace

// ---- loading, options ---------------------------------------------------------------

std::unique_ptr<Screen> make_details_screen(const Item& item) { return std::make_unique<DetailsScreen>(item); }

void reload_catalog() {
    if (g_loading) return;
    g_loading = true;
    auto next = std::make_shared<store::Catalog>();
    lib::async<bool>(
        nullptr, [next]() { next->load(); return true; },
        [next](bool&) {
            g_loading = false;
            store::set_catalog(next);
            lib::log_set_debug(next->config().debug_log);
            set_theme(next->config().theme == "arcade" ? ThemeId::Arcade : ThemeId::Modern);
            store::refresh_installed();
            forget_file_states();
            // The queue (restored in main) runs once the settings it uses
            // are loaded; start() does nothing after the first time.
            store::downloads().start();
            LOG("catalog: %zu titles", next->total());
            for (const auto& p : next->problems()) LOG("catalog: %s", p.c_str());
            app().switch_tab(0);
        },
        2);
}

void get_item(const Item& item) {
    // Already in hand: install it. Already downloading: open its page. Else the
    // chosen "get" method (download first, or let the console download).
    if (store::downloads().contains(item.url)) {
        app().push(make_details_screen(item));
        return;
    }
    if (store::Downloads::file_state(item) == store::Downloads::FileState::Complete) {
        app().toast(store::install_local(store::package_path(item), item).message);
        return;
    }
    get_item_direct(item);
}

void open_options_menu(store::Category category, bool installed) {
    auto cat = store::catalog();
    std::vector<MenuItem> items;
    if (installed) {
        // All, then each list that has installed titles, with how many.
        std::vector<std::pair<Category, int>> choices{{Category::Count, 0}};
        for (Category c : kTabOrder) {
            if (!installed_tab_list(c)) continue;
            std::set<std::string> ids;
            for (const auto& item : cat->items(c))
                if (!item.ps5 && !item.title_id.empty() && store::is_installed(item)) ids.insert(item.title_id);
            if (!ids.empty()) choices.push_back({c, (int)ids.size()});
        }
        if (g_installed_show != Category::Count) {
            bool listed = false;
            for (const auto& choice : choices) listed = listed || choice.first == g_installed_show;
            if (!listed) choices.push_back({g_installed_show, 0});
        }
        MenuItem show{"Show",
                      [choices] {
                          for (const auto& choice : choices)
                              if (choice.first == g_installed_show)
                                  return choice.first == Category::Count
                                             ? std::string("All")
                                             : std::string(store::category_name(choice.first)) + " (" +
                                                   std::to_string(choice.second) + ")";
                          return std::string("All");
                      },
                      nullptr,
                      [choices](int dir) {
                          int i = 0;
                          for (int k = 0; k < (int)choices.size(); ++k)
                              if (choices[k].first == g_installed_show) i = k;
                          const int n = (int)choices.size();
                          g_installed_show = choices[((i + dir) % n + n) % n].first;
                      }};
        show.stay_open = true;
        items.push_back(show);
    }
    if (category == Category::Homebrew) {
        items.push_back({"Homebrew category", [cat] { return cat->config().homebrew_category; }, [cat] {
            std::vector<MenuItem> groups;
            for (const auto& group : store::homebrew_categories()) {
                int count = 0;
                for (const auto& item : cat->items(Category::Homebrew))
                    if (group == "All" || item.homebrew_category == group) ++count;
                if (group == "Other" && count == 0) continue;
                groups.push_back({group, [cat, group, count] {
                    return std::to_string(count) + (cat->config().homebrew_category == group ? " / Selected" : "");
                }, [cat, group] {
                    cat->config().homebrew_category = group;
                    cat->save_config();
                }, nullptr});
            }
            open_menu("Homebrew categories", std::move(groups), "Choose the type of homebrew to browse.");
        }, nullptr});
    }
    auto changed = [cat] {
        cat->save_config();
        // The tab rebuilds its list with the new order and regions.
    };
    items.push_back({"Search", [] { return g_search.empty() ? std::string("Off") : g_search; },
                     [] {
                         open_keyboard("Search by name or title ID", g_search, KeyboardKind::Text,
                                       [](const std::string& text) { g_search = lib::trim(text); });
                     },
                     nullptr});
    if (!g_search.empty())
        items.push_back({"Clear search", nullptr, [] { g_search.clear(); }, nullptr});
    static const char* sorts[] = {"name", "size", "region", "titleid"};
    static const char* sort_names[] = {"Name", "Size", "Region", "Title ID"};
    MenuItem sort{"Sort by",
                  [cat] {
                      for (int i = 0; i < 4; ++i)
                          if (cat->config().sort == sorts[i]) return std::string(sort_names[i]);
                      return std::string("Name");
                  },
                  nullptr,
                  [cat, changed](int dir) {
                      int i = 0;
                      for (int k = 0; k < 4; ++k)
                          if (cat->config().sort == sorts[k]) i = k;
                      cat->config().sort = sorts[(i + dir + 4) % 4];
                      changed();
                  }};
    sort.stay_open = true;
    items.push_back(sort);
    MenuItem order{"Order", [cat] { return std::string(cat->config().ascending ? "Ascending" : "Descending"); }, nullptr,
                   [cat, changed](int) {
                       cat->config().ascending = !cat->config().ascending;
                       changed();
                   }};
    order.stay_open = true;
    items.push_back(order);
    // The Installed tab shows what is on the console whatever its region.
    for (const char* region : {"USA", "Europe", "Japan", "Asia"}) {
        if (installed) break;
        const std::string name = region;
        MenuItem r{"Show " + name,
                   [cat, name] {
                       const auto& rs = cat->config().regions;
                       return std::string(std::find(rs.begin(), rs.end(), name) != rs.end() ? "On" : "Off");
                   },
                   nullptr,
                   [cat, name, changed](int) {
                       auto& rs = cat->config().regions;
                       auto found = std::find(rs.begin(), rs.end(), name);
                       if (found != rs.end()) rs.erase(found);
                       else rs.push_back(name);
                       changed();
                   }};
        r.stay_open = true;
        items.push_back(r);
    }
    open_menu("Sort & filter", std::move(items), "Download, install and system options are in the Settings tab.");
}

// Where downloads are saved: the console or a USB drive.
std::string download_location_name(const std::string& path) { return store::destination_name(path); }
void choose_usb_drive(std::function<void()> selected) {
    auto cat = store::catalog();
    auto& cfg = cat->config();
    cfg.download_to_usb = true;
    const auto drives = store::connected_usb_drives();
    const auto use = [cat, selected](const std::string& path) {
        cat->config().download_path = path;
        cat->save_config();
        app().toast("Downloads will be saved to: " + download_location_name(path));
        if (selected) selected();
    };
    if (drives.size() == 1) { use(drives.front()); return; }
    cfg.download_path.clear();
    cat->save_config();
    if (drives.empty()) {
        app().toast("USB selected. Connect a USB drive before downloading.");
        return;
    }
    std::vector<MenuItem> items;
    for (const auto& drive : drives)
        items.push_back({download_location_name(drive), nullptr, [use, drive] { use(drive); }, nullptr});
    open_menu("Choose a USB drive", std::move(items), "More than one USB drive is connected.");
}

void open_download_location_menu() {
    auto cat = store::catalog();
    std::vector<MenuItem> items;
    items.push_back({"Console", [cat] { return cat->config().download_to_usb ? "" : "In use"; }, [cat] {
        cat->config().download_to_usb = false;
        cat->config().download_path = store::console_download_path();
        cat->save_config();
        app().toast("Downloads will be saved to: Console");
    }, nullptr});
    items.push_back({"USB drive", [cat] { return cat->config().download_to_usb ? "In use" : ""; },
        [] { choose_usb_drive(nullptr); }, nullptr});
    open_menu("Download destination", std::move(items),
        "One USB drive is selected automatically; choose a drive when several are connected.");
}

// ---- the Settings tab -----------------------------------------------------------------------

bool g_archive_signing_in = false;
void open_archive_account() {
    std::vector<MenuItem> items;
    if (g_archive_signing_in) { app().toast("Archive.org sign-in is still running."); return; }
    items.push_back({"Sign in", nullptr, [] {
        LOG("archive sign-in: opening email input");
        open_keyboard("Archive.org email", "", KeyboardKind::Text, [](const std::string& email) {
            if (lib::trim(email).empty()) return;
            LOG("archive sign-in: opening password input");
            open_keyboard("Archive.org password", "", KeyboardKind::Password, [email](const std::string& password) {
                if (password.empty() || g_archive_signing_in) return;
                g_archive_signing_in = true;
                LOG("archive sign-in: request queued");
                app().toast("Signing in to Archive.org...");
                lib::async<std::string>(nullptr, [email, password] { return lib::archive_sign_in(email, password); },
                    [](std::string& problem) {
                        g_archive_signing_in = false;
                        const std::string message = problem.empty() ? "Signed in to Archive.org. Retry your download from the Downloads tab." : problem;
                        open_menu(problem.empty() ? "Archive.org signed in" : "Archive.org sign-in failed",
                                  {{"Close", nullptr, [] {}, nullptr}}, message);
                    }, 4);
            });
        });
    }, nullptr});
    if (lib::archive_signed_in()) items.push_back({"Sign out", nullptr, [] {
        lib::archive_sign_out(); app().toast("Signed out of Archive.org.");
    }, nullptr});
    open_menu("Archive.org account", std::move(items),
        std::string(lib::archive_signed_in() ? "Signed in. " : "Signed out. ") +
        "Your password is not saved. You stay signed in on this console until you sign out. Sign-in only grants the access your account has.");
}

void open_content_sources() {
    auto cat = store::catalog();
    std::vector<MenuItem> items;
    for (int i = 0; i < (int)Category::Count; ++i) {
        const auto category = (Category)i;
        if (category == Category::PS5) continue;
        items.push_back({store::category_name(category),
            [cat, i] { return cat->config().content_urls[i].empty() ? "Local files" : "Web source"; },
            [cat, i, category] {
                open_keyboard(std::string(store::category_name(category)) + " content list URL (empty: local files)",
                    cat->config().content_urls[i], KeyboardKind::Url, [cat, i](const std::string& value) {
                        const std::string url = lib::trim(value);
                        if (!url.empty() && !lib::starts_with(url, "https://") && !lib::starts_with(url, "http://")) {
                            app().toast("Enter an http:// or https:// URL."); return;
                        }
                        cat->config().content_urls[i] = url;
                        if (!url.empty()) cat->config().populate_via_web = true;
                        cat->save_config();
                        reload_catalog();
                    });
            }, nullptr});
    }
    open_menu("Content sources", std::move(items), "Use your own content lists.");
}

class SettingsScreen : public Screen {
public:
    explicit SettingsScreen(int tab) : tab_(tab) { build(); }

    int tab() const override { return tab_; }
    std::vector<Hint> hints() const override { return {{kCircle, "Back"}, {kCross, "Select"}}; }

    void move(int dir) {
        int i = index_;
        for (int step = 0; step < (int)rows_.size(); ++step) {
            i += dir;
            if (i < 0 || i >= (int)rows_.size()) return;  // stop at the ends
            if (!rows_[i].heading) {
                index_ = i;
                return;
            }
        }
    }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().switch_tab(0);
            return;
        }
        build();  // values may have changed
        if (index_ >= (int)rows_.size()) index_ = first_selectable();
        if (in.repeat(kUp)) move(-1);
        if (in.repeat(kDown)) move(1);
        Row& r = rows_[index_];
        if (in.press(kCross) && r.activate) r.activate();
        if (r.adjust && (in.repeat(kLeft) || in.repeat(kRight))) {
            r.adjust(in.repeat(kRight) ? 1 : -1);
            build();
        }
    }

    void draw(Canvas& c) override {
        const Theme& t = theme();
        gfx::draw_text(c, Face::SansBold, 40, kMarginX, 150, "Settings", t.text);
        const int line = gfx::font_metrics(Face::SansSemiBold, kBodyPx).line_height;
        // The list has its own area, between the title and the footer: rows
        // scroll inside it and are never drawn over either.
        const int view_top = 214, view_h = kFooterTop - 14 - view_top;
        int total = 8, selected_top = 0;
        for (int i = 0; i < (int)rows_.size(); ++i) {
            if (i == index_) selected_top = total;
            total += rows_[i].heading ? kHeadingH : kRowH;
        }
        // The selected row is kept whole, with its section's heading when it is the first under it.
        int want_top = selected_top - 6;
        for (int i = index_ - 1; i >= 0 && rows_[i].heading; --i) want_top -= kHeadingH;
        if (want_top < scroll_) scroll_ = want_top;
        if (selected_top + kRowH > scroll_ + view_h) scroll_ = selected_top + kRowH - view_h;
        scroll_ = std::max(0, std::min(scroll_, std::max(0, total - view_h)));
        int y = view_top + 8 - scroll_;
        c.push_clip({kMarginX - 8, view_top, kContentW + 16, view_h});
        for (int i = 0; i < (int)rows_.size(); ++i) {
            const Row& r = rows_[i];
            if (r.heading) {
                gfx::draw_text(c, Face::SansBold, kSmallPx, kMarginX + 4, y + 16, r.label, t.accent);
                y += kHeadingH;
                continue;
            }
            const bool on = i == index_;
            const Rect row{kMarginX, y, kContentW, kRowH - 12};
            c.fill_rounded(row, 14, on ? t.surface_hi : gfx::with_alpha(t.surface, 170));
            if (on) c.stroke_rounded({row.x - 3, row.y - 3, row.w + 6, row.h + 6}, 17, 3, t.focus);
            const int ty = row.y + (row.h - line) / 2;
            const int lw = gfx::text_width(Face::SansSemiBold, kBodyPx, r.label);
            gfx::draw_text(c, Face::SansSemiBold, kBodyPx, row.x + 30, ty, r.label, t.text);
            if (!r.value.empty()) {
                std::string v = r.value;
                if (r.adjust) v = "\xE2\x80\xB9  " + v + "  \xE2\x80\xBA";
                // Long values (paths) are shortened so they never reach the label.
                const int avail = row.w - 60 - lw - 40;
                v = gfx::ellipsize(Face::SansSemiBold, kBodyPx, v, avail);
                const int vw = gfx::text_width(Face::SansSemiBold, kBodyPx, v);
                gfx::draw_text(c, Face::SansSemiBold, kBodyPx, row.right() - 30 - vw, ty, v, on ? t.text : t.muted);
            }
            y += kRowH;
        }
        c.pop_clip();
        if (total > view_h) {
            const int h = std::max(40, view_h * view_h / total);
            const int at = view_top + (int)((int64_t)(view_h - h) * scroll_ / std::max(1, total - view_h));
            c.fill_rounded({1920 - kMarginX + 14, view_top, 6, view_h}, 3, gfx::with_alpha(t.text, 30));
            c.fill_rounded({1920 - kMarginX + 14, at, 6, h}, 3, gfx::with_alpha(t.text, 140));
        }
    }

private:
    static constexpr int kRowH = 76;
    static constexpr int kHeadingH = 52;
    int scroll_ = 0;
    int first_selectable() const {
        for (int i = 0; i < (int)rows_.size(); ++i)
            if (!rows_[i].heading) return i;
        return 0;
    }
    struct Row {
        std::string label, value;
        std::function<void()> activate;
        std::function<void(int)> adjust;
        bool heading = false;
    };

    void build() {
        auto cat = store::catalog();
        auto& cfg = cat->config();
        rows_.clear();
        auto onoff = [](bool b) { return std::string(b ? "On" : "Off"); };
        if (index_ < 0) index_ = 1;  // the first real row, after the heading

        rows_.push_back({"Downloads", "", nullptr, nullptr, true});
        rows_.push_back({"Download destination", cfg.download_to_usb ? "USB drive" : "Console", [] { open_download_location_menu(); }, nullptr, false});
        rows_.push_back({"Connections per download", std::to_string(cfg.connections) + (cfg.connections == 1 ? " (slowest)" : ""), nullptr,
                         [cat](int dir) {
                             cat->config().connections = std::max(1, std::min(8, cat->config().connections + dir));
                             cat->save_config();
                         },
                         false});
        rows_.push_back({"Install after downloading", onoff(cfg.install_after), nullptr,
            [cat](int) { cat->config().install_after = !cat->config().install_after; cat->save_config(); }, false});
        rows_.push_back({"Delete PKG after successful install", onoff(cfg.cleanup_after_install), nullptr,
            [cat](int) { cat->config().cleanup_after_install = !cat->config().cleanup_after_install; cat->save_config(); }, false});
        // What happens around starting a game: the launcher's own checks, and
        // PlayStation Network updates that would replace or break an fPKG.
        rows_.push_back({"Pre-flight checks", "", nullptr, nullptr, true});
        rows_.push_back({"Skip launcher preflight checks", onoff(cfg.skip_game_update_checks), nullptr,
            [cat](int) { cat->config().skip_game_update_checks = !cat->config().skip_game_update_checks; cat->save_config(); }, false});
        rows_.push_back({"Block PlayStation Network updates",
            !cfg.block_sony_updates ? "Off" : store::service_blocker_active() ? "On (background)" : "On (while FPKGi+ is open)",
            nullptr,
            [cat](int) {
                cat->config().block_sony_updates = !cat->config().block_sony_updates;
                cat->save_config();  // the background service reads it again within a few seconds
            }, false});
        rows_.push_back({"About update blocking", "Help", [] {
            open_menu("PlayStation Network updates", {{"Close", nullptr, [] {}, nullptr}},
                "An official update from PlayStation Network does not run on a game installed as an fPKG: the game stops "
                "starting. With Block PlayStation Network updates on, FPKGi+ removes such downloads from the console's queue "
                "as they appear and tells you. It runs in the background downloader, so it works with FPKGi+ closed and "
                "while you play; without the background downloader it works only while FPKGi+ is open. Your own installs, "
                "Package Installer installs, system software updates and games you own on disc or from the Store are left alone.\n\n"
                "To stop the console downloading them at all, turn off Application Update Files in PS4 Settings > System > "
                "Automatic Downloads.");
        }, nullptr, false});
        rows_.push_back({"Content", "", nullptr, nullptr, true});
        rows_.push_back({"Content sources", "", [] { open_content_sources(); }, nullptr, false});
        rows_.push_back({"Archive.org account", g_archive_signing_in ? "Signing in..." : lib::archive_signed_in() ? "Signed in" : "Signed out",
                         [] { open_archive_account(); }, nullptr, false});
        rows_.push_back({"Populate via web", onoff(cfg.populate_via_web), nullptr,
                         [cat](int) {
                             cat->config().populate_via_web = !cat->config().populate_via_web;
                             cat->save_config();
                         },
                         false});
        rows_.push_back({"Reload content lists", std::to_string(cat->total()) + " titles",
                         [] { reload_catalog(); app().toast("Reloading\xE2\x80\xA6"); }, nullptr, false});

        rows_.push_back({"Appearance", "", nullptr, nullptr, true});
        rows_.push_back({"Theme", cfg.theme == "arcade" ? "Retro arcade" : "Modern", nullptr,
                         [cat](int) {
                             cat->config().theme = cat->config().theme == "arcade" ? "modern" : "arcade";
                             set_theme(cat->config().theme == "arcade" ? ThemeId::Arcade : ThemeId::Modern);
                             cat->save_config();
                         },
                         false});

        rows_.push_back({"System", "", nullptr, nullptr, true});
        // The background downloader (store/service.h).
        rows_.push_back({"Background downloads", onoff(cfg.background_downloads), nullptr,
            [cat](int) {
                cat->config().background_downloads = !cat->config().background_downloads;
                cat->save_config();
                app().toast(cat->config().background_downloads
                    ? "Downloads will run in the background from the next time FPKGi+ opens."
                    : "Downloads will run in FPKGi+ from the next time it opens. Remove the background downloader below to stop it now.");
            }, false});
        rows_.push_back({"Background downloader", store::service_status(), [] {
            open_menu("Background downloader", {
                {"Restart", nullptr, [] { app().toast(store::service_restart()); }, nullptr},
                {"Remove", nullptr, [] {
                    confirm("Remove the background downloader?",
                        "It is stopped and deleted from the system partition. A download it was running is paused "
                        "and continues in FPKGi+.",
                        "Remove", [] { app().toast(store::service_remove()); });
                }, nullptr},
            }, "Downloads go on after FPKGi+ is closed and while you play. In rest mode they pause, and continue by "
               "themselves when the console wakes. It is installed in the console's "
               "system partition, as Itemzflow installs its daemon.");
        }, nullptr, false});
        rows_.push_back({"Debug logging", onoff(cfg.debug_log), nullptr,
                         [cat](int) {
                             cat->config().debug_log = !cat->config().debug_log;
                             lib::log_set_debug(cat->config().debug_log);
                             cat->save_config();
                         },
                         false});
        rows_.push_back({"Software update", update_value(), [] { update_action(); }, nullptr, false});
        rows_.push_back({"About FPKGi+", "", [] { open_about(); }, nullptr, false});
    }

    static std::string update_value() {
        using S = store::updater::State;
        switch (store::updater::state()) {
            case S::Idle: return "Check now";
            case S::Checking: return "Checking\xE2\x80\xA6";
            case S::Current: return "Up to date (" + store::updater::current_version() + ")";
            case S::Available: return store::updater::available_version() + " available";
            case S::Downloading: {
                uint64_t done = 0, total = 0;
                store::updater::progress(done, total);
                return total ? std::to_string((int)(done * 100 / total)) + "%" : lib::format_bytes(done);
            }
            case S::Downloaded: return "Ready to install";
            case S::Failed: return "Failed: select to try again";
        }
        return std::string();
    }

    // FPKGi+ only downloads and checks its update; the user installs it from
    // the Package Installer after closing the app (it never installs itself).
    static void update_action() {
        using S = store::updater::State;
        const std::string where = std::string(store::updater::kFile);
        switch (store::updater::state()) {
            case S::Available:
                open_menu("Download FPKGi+ " + store::updater::available_version() + "?",
                          {{"Download", nullptr, [] { store::updater::download(); }, nullptr},
                           {"Not now", nullptr, [] {}, nullptr}},
                          "It is saved to " + where + " and checked. Then close FPKGi+ and install it from "
                          "Debug Settings > Game > Package Installer. FPKGi+ does not install its own updates.");
                return;
            case S::Downloading:
                open_menu("Stop the download?",
                          {{"Stop", nullptr, [] { store::updater::cancel(); }, nullptr},
                           {"Keep downloading", nullptr, [] {}, nullptr}}, "");
                return;
            case S::Downloaded:
                open_menu("Update ready", {{"Close", nullptr, [] {}, nullptr}},
                          "FPKGi+ " + store::updater::available_version() + " is saved to " + where +
                          ". Close FPKGi+, then install it from Debug Settings > Game > Package Installer.");
                return;
            case S::Checking:
                return;
            case S::Failed:
                app().toast(store::updater::error());
                store::updater::check();
                return;
            default:
                store::updater::check();
        }
    }

    static void open_about();

    int tab_;
    std::vector<Row> rows_;
    int index_ = 1;  // start on the first real row (0 is a heading)
};

void SettingsScreen::open_about() {
    std::vector<MenuItem> items;
    items.push_back({"Close", nullptr, [] {}, nullptr});
    const std::string body =
        "FPKGi+ " LIB_VERSION_STR
        " - a standalone PS4 catalogue, downloader and package installer. "
        "No games or PKGs are included. It ships only a list of open-source homebrew from GitHub; "
        "add your own content sources in Settings for anything else. "
        "Its settings, content lists, metadata and downloads live in /data/FPKGiPlus. "
        "Installation uses the console service or GoldHEN's install folder.\n\n"
        "Supports FPKGi-compatible content lists. "
        "Based on Librarian by jw-notes - https://github.com/jw-notes/Librarian. "
        "Service credentials adapted from Itemzflow's libjbc helper by LM, Sleirsgoevy and Specter; "
        "https://github.com/LightningMods/Itemzflow. "
        "Free software under the GNU GPL v3; see the licenses folder in the package.";
    open_menu("About FPKGi+", std::move(items), body);
}

// ---- the header's tabs: categories, Installed, then Downloads and Settings ----------------

std::vector<std::string> tab_names() {
    std::vector<std::string> names;
    for (Category c : visible_categories()) names.push_back(store::category_name(c));
    if (names.empty()) names.push_back("Store");
    else names.push_back("Installed");  // only once there are lists to match installs against
    names.push_back("Downloads");
    names.push_back("Settings");
    return names;
}

std::unique_ptr<Screen> make_tab_screen(int tab) {
    const auto cats = visible_categories();
    const int n = (int)cats.size();
    if (n == 0) {  // Store, Downloads, Settings
        if (tab == 2) return std::make_unique<SettingsScreen>(tab);
        if (tab == 1) return std::make_unique<DownloadsScreen>(tab);
        return std::make_unique<StoreScreen>(tab, Category::Games);
    }
    if (tab == n + 2) return std::make_unique<SettingsScreen>(tab);
    if (tab == n + 1) return std::make_unique<DownloadsScreen>(tab);
    if (tab == n) return std::make_unique<StoreScreen>(tab, Category::Games, true);
    return std::make_unique<StoreScreen>(tab, cats[tab]);
}

}  // namespace ui
