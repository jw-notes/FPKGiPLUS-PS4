// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The background downloader (FPKP00002).
//
// A console background service, installed and started by FPKGi+ (see
// store/service.h). It runs FPKGi+'s own download queue (store/downloads.cpp,
// the same code the app uses), so downloads go on after FPKGi+ is closed and
// while other apps and games run. Rest mode suspends it (as it does every app
// and background service; sceLncUtilBlockAppSuspend does not prevent that),
// so a download pauses there and continues when the console wakes. It talks
// to the app through <data>/service/ (downloads.h):
// it applies the app's commands and writes its list and progress for it.
// Installing stays with the app, which installs a finished download when it
// is open or the next time it is opened.
//
// It stops when <data>/service/stop appears (FPKGi+ asks for that before it
// replaces or removes the service).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include "core/fs.h"
#include "core/log.h"
#include "core/usb_ftp.h"
#include "net/archive.h"
#include "net/http.h"
#include "platform/platform.h"
#include "store/catalog.h"
#include "store/downloads.h"

namespace {

// A console notification: shown over whatever is on screen (a game, another
// app, the home screen), as the console's own download notices are.
void popup(const std::string& text) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof req);
    req.type = NotificationRequest;
    req.targetId = -1;
    // FPKGi+'s own icon, as the console keeps it for the installed app (the
    // notification is drawn by the system, which reads the file itself).
    req.useIconImageUri = 1;
    snprintf(req.iconUri, sizeof req.iconUri, "%s", "/user/appmeta/FPKP00001/icon0.png");
    snprintf(req.message, sizeof req.message, "%s", text.c_str());
    const int rc = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    LOG("service: notification \"%s\" -> 0x%08x", text.c_str(), (unsigned)rc);
}

// Rest mode and waking up. Going into rest mode the service runs on for a few
// seconds with no user on screen and is then suspended; after waking, the
// console's interface and the network are still starting for a while.
// Notifications wait until a user has been on screen for kQuietSeconds, and
// a download that fails in that time (the network drops as the console goes
// to sleep and wakes) is tried again instead of being reported.
constexpr time_t kQuietSeconds = 60;
constexpr int kNetworkRetries = 5;
constexpr time_t kRetryDelaySeconds = 30;

struct Watch {
    std::map<uint64_t, store::JobState> running;  // downloads seen running this session
    std::map<std::string, int> retries;            // by URL
    std::map<std::string, time_t> retry_at;        // by URL
    std::vector<std::string> notices;              // waiting for the screen
    time_t quiet_until = 0;
    time_t last_wall = 0;
    bool user_on_screen = true;
};

bool user_on_screen() {
    int32_t user = -1;
    return sceUserServiceGetForegroundUser(&user) == 0 && user != -1 && user != 0;
}

// Failures worth trying again: the connection, not the file or the disk.
bool network_failure(const std::string& message) {
    for (const char* final_reason : {"Not enough free space", "did not send a PS4 package", "(401)", "(403)", "(404)", "(410)"})
        if (message.find(final_reason) != std::string::npos) return false;
    return true;
}

void watch_screen(Watch& w) {
    const time_t wall = time(nullptr);
    if (w.last_wall && wall - w.last_wall > 5) {
        LOG("service: resumed after %lld s without running (rest mode or suspended)", (long long)(wall - w.last_wall));
        w.quiet_until = std::max(w.quiet_until, wall + kQuietSeconds);
    }
    w.last_wall = wall;
    const bool on_screen = user_on_screen();
    if (on_screen != w.user_on_screen) {
        LOG("service: %s", on_screen ? "a user is on screen again" : "no user on screen (rest mode): notifications wait");
        w.user_on_screen = on_screen;
    }
    if (!on_screen) w.quiet_until = std::max(w.quiet_until, wall + kQuietSeconds);
    if (wall >= w.quiet_until && !w.notices.empty()) {
        for (const auto& text : w.notices) popup(text);
        w.notices.clear();
    }
}

void notify(Watch& w, const std::string& text) {
    if (time(nullptr) < w.quiet_until || !w.user_on_screen) {
        LOG("service: notification held until the console is awake: \"%s\"", text.c_str());
        w.notices.push_back(text);
        return;
    }
    popup(text);
}

// Downloads seen running in this session: when one finishes or fails, the
// user is told, wherever they are. One restored as already finished is not
// announced again.
void announce_changes(Watch& w) {
    const time_t wall = time(nullptr);
    for (const store::Job& job : store::downloads().snapshot()) {
        auto seen = w.running.find(job.id);
        if (job.state == store::JobState::Downloading) {
            w.running[job.id] = job.state;
            continue;
        }
        if (seen == w.running.end()) continue;
        w.running.erase(seen);
        const std::string& url = job.item.url;
        if (job.state == store::JobState::Done) {
            w.retries.erase(url);
            notify(w, job.item.name + " has finished downloading.");
        } else if (job.state == store::JobState::Failed) {
            int& tries = w.retries[url];
            if (network_failure(job.message) && tries < kNetworkRetries) {
                ++tries;
                w.retry_at[url] = wall + kRetryDelaySeconds;
                LOG("service: %s failed (%s); trying again in %lld s (%d of %d)", job.item.title_id.c_str(), job.message.c_str(),
                    (long long)kRetryDelaySeconds, tries, kNetworkRetries);
            } else {
                w.retries.erase(url);
                notify(w, job.item.name + " did not download: " + job.message);
            }
        }
    }
    for (auto it = w.retry_at.begin(); it != w.retry_at.end();) {
        if (wall < it->second) { ++it; continue; }
        std::string problem;
        if (!store::downloads().resume(it->first, &problem)) LOG("service: retry not started: %s", problem.c_str());
        it = w.retry_at.erase(it);
    }
}

}  // namespace

int main() {
    platform::init_service();
    lib::log_set_sync(true);  // the last lines survive a crash or a power cut
    sceSystemServiceRegisterDaemon();
    if (!lib::http_init()) LOG("service: network init failed");
    // It stays in its sandbox. A test build that left it (with the Itemzflow
    // credential helper, as the app does) turned the console off when FPKGi+
    // was closed during a download; inside it, /data and the network work.
    // USB drives are reached through GoldHEN's FTP server, as the app does
    // when it cannot leave the sandbox.
    lib::usb_ftp::enable("127.0.0.1:2121", "/mnt/usb");
    const std::string data = platform::data_root();
    lib::archive_restore_session(lib::join_path(data, "archive-session.json"));
    const std::string config_path = lib::join_path(data, "config.json");
    std::string config_text;
    lib::read_file(config_path, config_text, 1 << 20);
    store::catalog()->load_settings();

    const std::string stop_path = lib::join_path(store::Downloads::service_dir(), "stop");
    lib::make_dirs(store::Downloads::service_dir());
    lib::remove_file(stop_path);

    auto& queue = store::downloads();
    queue.resume_interrupted(true);
    queue.restore();
    queue.start();
    queue.service_write_state(true);
    LOG("service: running, pid %d, sandbox %d", getpid(), sceKernelIsInSandbox());
    popup("FPKGi+ background downloader started");

    uint64_t settings_checked = platform::now_ms();
    Watch watch;
    for (;;) {
        queue.service_apply_commands();
        watch_screen(watch);
        announce_changes(watch);
        // Settings changed in the app (connections, destination): read again.
        const uint64_t now = platform::now_ms();
        if (now - settings_checked >= 5000) {
            settings_checked = now;
            std::string text;
            if (lib::read_file(config_path, text, 1 << 20) && text != config_text) {
                config_text = text;
                store::catalog()->load_settings();
                LOG("service: settings reloaded");
            }
        }
        queue.service_write_state(false);
        if (lib::file_exists(stop_path)) {
            lib::remove_file(stop_path);
            LOG("service: stop requested");
            break;
        }
        platform::sleep_ms(250);
    }
    queue.stop();  // a running download keeps its partial file and is resumed next time
    queue.service_write_state(true);
    lib::http_shutdown();
    LOG("service: stopped");
    return sceSystemServiceLoadExec("exit", nullptr);
}
