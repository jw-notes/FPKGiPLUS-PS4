// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// FPKGi+: a store-style front end for FPKGi's content lists on the PS4.
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "core/log.h"
#include "core/worker.h"
#include "gfx/font.h"
#include "net/archive.h"
#include "net/http.h"
#include "platform/platform.h"
#include "store/covers.h"
#include "store/downloads.h"
#include "store/installer.h"
#include "store/updater.h"
#include "ui/app.h"
#include "ui/screens.h"

#ifdef LIB_HOST
#include "platform/host.h"
#endif

int main(int argc, char** argv) {
#ifdef LIB_HOST
    // fpkgi-plus [--script "steps"] [--shots dir]
    std::string script, shots = "build/host/shots";
    for (int i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--script")) script = argv[++i];
        else if (!strcmp(argv[i], "--shots")) shots = argv[++i];
    }
#endif
    if (!platform::init()) {
        LOG("platform init failed");
        platform::exit_app();
    }
    lib::log_previous_crash(lib::join_path(platform::data_root(), "crash.log"));
    platform::install_crash_handler();
#ifdef LIB_HOST
    host::set_script(script, shots);
#endif
    LOG("FPKGi+ %s, %s",
#ifdef LIB_VERSION
        LIB_VERSION,
#else
        "dev",
#endif
        platform::memory_report().c_str());
    if (!gfx::fonts_init(lib::join_path(platform::assets_root(), "fonts"))) LOG("some fonts are missing");
    // Initialise TLS and load its trust anchors while the original app
    // credentials and sandbox certificate paths are still available.
    if (!lib::http_init()) LOG("network init failed");
    // After the package's fonts are loaded (leaving the sandbox can change
    // what /app0 means) and before any thread starts: GoldHEN's jailbreak, for
    // /user/app and the install service.
    platform::enable_usb_access();
    if (!store::installer_init()) LOG("installer: %s", store::installer_problem().c_str());
    lib::make_dirs(lib::join_path(platform::data_root(), "ContentJSONs"));
    lib::make_dirs(lib::join_path(platform::data_root(), "Downloads"));
    lib::archive_restore_session(lib::join_path(platform::data_root(), "archive-session.json"));
    if (!platform::video_start(0)) {
        LOG("video could not start");
        platform::exit_app();
    }
    lib::workers_start(4);
    store::covers_init();
    // The queue left by the last session, before anything can be added to it.
    // Its worker starts when the catalogue (and so the settings) has loaded.
    store::downloads().restore();
    ui::reload_catalog();
    // A newer FPKGi+ on GitHub: checked once the app has settled.
    store::updater::check(30000);

    ui::app().run();
    ui::app().close_all();

    store::updater::shutdown();  // an unfinished update download is discarded
    store::downloads().stop();  // a running download keeps its .part file, to be continued
    for (int i = 0; i < 300 && lib::jobs_in_flight() > 0; ++i) {
        lib::run_completions();
        platform::sleep_ms(10);
    }
    lib::workers_stop();
    lib::http_shutdown();
    platform::shutdown();
    platform::exit_app();
}
