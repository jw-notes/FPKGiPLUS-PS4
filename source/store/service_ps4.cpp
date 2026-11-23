// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Installs, updates, starts and removes the background downloader. See store/service.h.
#include "store/service.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>

#include <orbis/SystemService.h>
#include <orbis/libkernel.h>

#include <vector>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "platform/platform.h"
#include "platform/sandbox_ps4.h"
#include "store/downloads.h"

extern "C" {
int orbis_syscall(int num, ...);  // goldhen_sdk/source/Syscall.c
int32_t sceLncUtilInitialize(void);
int32_t sceLncUtilGetAppId(const char* title_id);
int32_t sceLncUtilKillApp(uint32_t app_id);
}

namespace store {

namespace {

const char kTitle[] = "FPKP00002";
const char kDir[] = "/system/vsh/app/FPKP00002";
const char kTemplateSfo[] = "/system/vsh/app/NPXS21007/sce_sys/param.sfo";
constexpr int kSysNmount = 378;
constexpr int kMntRdonly = 0x1;
constexpr int kMntUpdate = 0x10000;

// What goes into the service's folder: (from FPKGi+'s package, to the folder).
struct Copy { const char* from; const char* to; };
const Copy kFiles[] = {
    {"service/service.self", "eboot.bin"},
    {"sce_module/libc.prx", "sce_module/libc.prx"},
    {"sce_module/libSceFios2.prx", "sce_module/libSceFios2.prx"},
    {"assets/cacert.pem", "assets/cacert.pem"},
    {"assets/godaddy-secure-g2.pem", "assets/godaddy-secure-g2.pem"},
    {"assets/letsencrypt-gen-y.pem", "assets/letsencrypt-gen-y.pem"},
};
// Left by earlier test builds; removed with the service.
const char* const kOldFiles[] = {"Media/fpkgi-jbc.prx", "sce_module/fpkgi-jbc.prx"};

std::string g_problem;

std::string hex(int32_t v) {
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08X", (unsigned)v);
    return buf;
}

// /system remounted read-write (or back to read-only), as Itemzflow does for its daemon.
bool remount_system(bool writable) {
    struct Pair { const char* name; const char* value; };
    const Pair pairs[] = {{"fstype", "exfatfs"}, {"fspath", "/system"}, {"from", "/dev/da0x4.crypt"},
                          {"large", "yes"}, {"timezone", "static"}, {"async", ""}, {"ignoreacl", ""},
                          {"dirmask", "511"}, {"mask", "511"}};
    std::vector<iovec> iov;
    for (const Pair& p : pairs) {
        iov.push_back({(void*)p.name, strlen(p.name) + 1});
        iov.push_back({(void*)p.value, strlen(p.value) + 1});
    }
    const int rc = orbis_syscall(kSysNmount, iov.data(), (unsigned)iov.size(), kMntUpdate | (writable ? 0 : kMntRdonly));
    LOG("service: remount /system %s -> %d (errno %d)", writable ? "read-write" : "read-only", rc, rc < 0 ? errno : 0);
    return rc == 0;
}

// The package's own files: /app0 inside the sandbox, the mounted package outside it.
std::string package_file(const char* relative) {
    for (const char* root : {"/app0/", "/mnt/sandbox/pfsmnt/FPKP00001-app0/"}) {
        const std::string path = std::string(root) + relative;
        if (lib::file_exists(path)) return path;
    }
    return "";
}

bool copy_file(const std::string& from, const std::string& to) {
    std::string data;
    if (!lib::read_file(from, data, 64 << 20)) { LOG("service: cannot read %s", from.c_str()); return false; }
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { LOG("service: cannot write %s (errno %d)", to.c_str(), errno); return false; }
    const bool ok = fwrite(data.data(), 1, data.size(), out) == data.size();
    return fclose(out) == 0 && ok;
}

bool same_file(const std::string& a, const std::string& b) {
    std::string x, y;
    return lib::read_file(a, x, 64 << 20) && lib::read_file(b, y, 64 << 20) && x == y;
}

int32_t running_app_id() {
    const int32_t id = sceLncUtilGetAppId(kTitle);
    return ((uint32_t)id & ~0xFFFFFFu) == 0x60000000u ? id : -1;
}

// The service's last heartbeat, in wall-clock seconds (-1: none).
int64_t last_beat() {
    std::string text;
    if (!lib::read_file(lib::join_path(Downloads::service_dir(), "state.json"), text, 8 << 20)) return -1;
    bool ok = false;
    const lib::Json root = lib::Json::parse(text, &ok);
    return ok ? root["beat"].integer(-1) : -1;
}

bool wait_until_gone(int ms) {
    for (int waited = 0; waited < ms && running_app_id() >= 0; waited += 250) platform::sleep_ms(250);
    return running_app_id() < 0;
}

// Asks it to stop (it finishes writing its state), then makes sure it is gone.
bool stop_service() {
    sceLncUtilInitialize();
    const int32_t id = running_app_id();
    if (id < 0) return true;
    lib::make_dirs(Downloads::service_dir());
    lib::write_file_atomic(lib::join_path(Downloads::service_dir(), "stop"), "stop\n");
    lib::write_file_atomic(lib::join_path(platform::data_root(), "daemon-test.stop"), "stop\n");  // an earlier test build
    if (!wait_until_gone(8000)) {
        const int32_t rc = sceLncUtilKillApp((uint32_t)id);
        LOG("service: did not stop by itself; kill %s -> %s", hex(id).c_str(), hex(rc).c_str());
        wait_until_gone(5000);
    }
    lib::remove_file(lib::join_path(Downloads::service_dir(), "stop"));
    lib::remove_file(lib::join_path(platform::data_root(), "daemon-test.stop"));
    return running_app_id() < 0;
}

bool installed_current() {
    const std::string dir = kDir;
    for (const Copy& c : kFiles) {
        const std::string source = package_file(c.from), target = dir + "/" + c.to;
        if (source.empty() || !lib::file_exists(target)) return false;
    }
    return lib::file_exists(dir + "/sce_sys/param.sfo") && same_file(package_file(kFiles[0].from), dir + "/eboot.bin");
}

bool install_files() {
    for (const Copy& c : kFiles)
        if (package_file(c.from).empty()) { g_problem = std::string("this build has no ") + c.from; return false; }
    if (!lib::file_exists(kTemplateSfo)) { g_problem = "the console's background-service template is missing"; return false; }
    if (!stop_service()) { g_problem = "the running background downloader would not stop"; return false; }
    if (!remount_system(true)) { g_problem = "the system partition could not be made writable"; return false; }
    const std::string dir = kDir;
    bool ok = lib::make_dirs(dir + "/sce_sys") && lib::make_dirs(dir + "/sce_module") && lib::make_dirs(dir + "/assets");
    for (const Copy& c : kFiles) ok = ok && copy_file(package_file(c.from), dir + "/" + c.to);
    ok = ok && copy_file(kTemplateSfo, dir + "/sce_sys/param.sfo");
    for (const char* old : kOldFiles) lib::remove_file(dir + "/" + old);
    lib::remove_dir(dir + "/Media");
    remount_system(false);
    LOG("service: installed in %s -> %d", kDir, ok);
    if (!ok) g_problem = "copying it to the system partition failed";
    return ok;
}

bool launch_and_wait() {
    const int64_t asked = (int64_t)time(nullptr);
    LncAppParam param{};
    param.size = sizeof(param);
    param.user_id = (uint32_t)-1;
    param.LaunchAppCheck_flag = LaunchApp_SkipSystemUpdate;
    const int32_t rc = sceLncUtilLaunchApp(kTitle, nullptr, &param);
    LOG("service: launch -> %s", hex(rc).c_str());
    if (rc < 0) { g_problem = "the console would not start it (" + hex(rc) + ")"; return false; }
    for (int waited = 0; waited < 10000; waited += 250) {
        if (last_beat() >= asked) return true;
        platform::sleep_ms(250);
    }
    g_problem = "it started but did not answer";
    return false;
}

}  // namespace

bool service_start() {
    g_problem.clear();
    if (!platform::sandbox::lifted()) { g_problem = "FPKGi+ is in the sandbox (GoldHEN is needed)"; return false; }
    sceLncUtilInitialize();
    if (!installed_current() && !install_files()) {
        LOG("service: not used: %s", g_problem.c_str());
        return false;
    }
    if (running_app_id() >= 0) {
        // Running: it writes its state at least every 5 s.
        const int64_t beat = last_beat();
        if (beat >= 0 && (int64_t)time(nullptr) - beat <= 15) {
            LOG("service: already running");
            downloads().use_service(true);
            return true;
        }
        LOG("service: running but silent; starting it again");
        stop_service();
    }
    if (!launch_and_wait()) {
        LOG("service: not used: %s", g_problem.c_str());
        return false;
    }
    LOG("service: started");
    downloads().use_service(true);
    return true;
}

std::string service_status() {
    // Asked every frame on the Settings screen: the console is asked every 2 s.
    static std::string cached;
    static uint64_t asked_at = 0;
    const uint64_t now = platform::now_ms();
    if (!cached.empty() && now - asked_at < 2000) return cached;
    asked_at = now;
    if (!platform::sandbox::lifted()) cached = "Unavailable (needs GoldHEN)";
    else if (running_app_id() >= 0) cached = downloads().using_service() ? "Running" : "Running (not used until restart)";
    else if (!lib::file_exists(std::string(kDir) + "/eboot.bin")) cached = "Not installed";
    else cached = g_problem.empty() ? "Not running" : "Not running (" + g_problem + ")";
    return cached;
}

std::string service_restart() {
    if (!platform::sandbox::lifted()) return "The background downloader needs GoldHEN.";
    if (!downloads().using_service()) return "Downloads run in FPKGi+ this session. Close and reopen FPKGi+ to use the background downloader.";
    g_problem.clear();
    if (!stop_service()) return "The background downloader would not stop.";
    if (!launch_and_wait()) return "The background downloader did not start again: " + g_problem + ".";
    return "Background downloader restarted. Downloads continue.";
}

std::string service_remove() {
    if (!platform::sandbox::lifted()) return "The background downloader needs GoldHEN.";
    if (!stop_service()) return "The background downloader would not stop; it was not removed.";
    if (downloads().using_service()) {
        // The queue it saved runs in FPKGi+ again from here.
        downloads().use_service(false);
        downloads().restore();
        downloads().start();
    }
    if (!lib::dir_exists(kDir)) return "The background downloader is not installed.";
    if (!remount_system(true)) return "Stopped, but the system partition could not be made writable to remove it.";
    const std::string dir = kDir;
    for (const Copy& c : kFiles) lib::remove_file(dir + "/" + c.to);
    for (const char* old : kOldFiles) lib::remove_file(dir + "/" + old);
    lib::remove_file(dir + "/sce_sys/param.sfo");
    for (const char* sub : {"/sce_module", "/assets", "/sce_sys", "/Media"}) lib::remove_dir(dir + sub);
    const bool removed = lib::remove_dir(dir);
    remount_system(false);
    LOG("service: removed %s -> %d", kDir, removed);
    return removed ? "Background downloader removed. Turn off Background downloads too, or it is installed again the next time FPKGi+ opens."
                   : "Stopped, but its folder could not be removed (see the log).";
}

}  // namespace store
