// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The console's install service (libSceBgft) and libSceAppInstUtil. They are
// loaded at run time, not linked: a sandboxed app may not load them, and a
// missing import would stop FPKGi+ from starting at all. Without them the app
// still browses and downloads; only installing is unavailable.
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>

#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <orbis/_types/bgft.h>

#include <map>
#include <set>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/text.h"
#include "platform/platform.h"
#include "platform/sandbox_ps4.h"
#include "store/catalog.h"
#include "store/installer.h"
#include "store/bgft_abi.h"

extern "C" int32_t sceLncUtilInitialize(void);

namespace store {

namespace {

using InitFn = int32_t (*)();
using BgftInitFn = int32_t (*)(OrbisBgftInitParams*);
using RegisterExFn = int32_t (*)(bgft::StorageParameters*, int32_t*);
using RegisterDebugFn = int32_t (*)(bgft::StorageParameters*, int32_t*);
using TitleFromPkgFn = int32_t (*)(const char*, char*, int32_t*);
using StartFn = int32_t (*)(int32_t);
// The SDK's progress structure uses 32-bit lengths. The native ABI (also
// used by Itemzflow) has 64-bit lengths and is 64 bytes on PS4.
using NativeBgftProgress = bgft::Progress;
using ProgressFn = int32_t (*)(int32_t, NativeBgftProgress*);
ProgressFn g_progress = nullptr;

InitFn g_app_inst_init = nullptr;
TitleFromPkgFn g_title_from_pkg = nullptr;
int g_app_inst_module = -1;
bool g_app_inst_ready = false;
BgftInitFn g_bgft_init = nullptr;
RegisterExFn g_register_ex = nullptr;
RegisterDebugFn g_register_debug = nullptr;
StartFn g_start = nullptr;
StartFn g_stop = nullptr, g_unregister = nullptr;
bool g_ready = false;
bool g_attempted = false;
std::string g_problem = "The install service has not been loaded.";

int load(const char* path) {
    const int32_t handle = (int32_t)sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    if (handle < 0) LOG("installer: loading %s failed 0x%08x", path, (unsigned)handle);
    return handle;
}

// A system library by file name. Outside the sandbox (the app, after
// GoldHEN) it is under /system/common/lib; inside it (the background
// service) the same folder is mounted at /<sandbox word>/common/lib.
int load_system_library(const char* file) {
    int handle = load(("/system/common/lib/" + std::string(file)).c_str());
    if (handle >= 0) return handle;
    const char* word = sceKernelGetFsSandboxRandomWord();
    if (word && *word) {
        const std::string path = "/" + std::string(word) + "/common/lib/" + file;
        handle = load(path.c_str());
        LOG("installer: %s from the sandbox's system folder -> 0x%08x", file, (unsigned)handle);
    }
    return handle;
}

template <typename T>
bool sym(int handle, const char* name, T& out) {
    void* p = nullptr;
    const int rc = sceKernelDlsym(handle, name, &p);
    if (rc < 0 || !p) {
        LOG("installer: %s not found (0x%08x)", name, (unsigned)rc);
        return false;
    }
    out = (T)p;
    return true;
}

std::string hex(int32_t rc) {
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08X", (unsigned)rc);
    return buf;
}

}  // namespace

bool installer_init() {
    if (g_ready) return true;
    if (g_attempted) return false;
    g_attempted = true;
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_BGFT);
    const int inst = load_system_library("libSceAppInstUtil.sprx");
    g_app_inst_module = inst;
    const int bgft = load_system_library("libSceBgft.sprx");
    if (inst < 0 || bgft < 0) {
        g_problem = "The console's install service could not be loaded (GoldHEN is needed).";
        return false;
    }
    bool ok = sym(inst, "sceAppInstUtilInitialize", g_app_inst_init);
    sym(inst, "sceAppInstUtilGetTitleIdFromPkg", g_title_from_pkg);
    ok = sym(bgft, "sceBgftServiceIntInit", g_bgft_init) && ok;
    ok = sym(bgft, "sceBgftServiceIntDownloadRegisterTaskByStorageEx", g_register_ex) && ok;
    ok = sym(bgft, "sceBgftServiceDownloadStartTask", g_start) && ok;
    sym(bgft, "sceBgftServiceDownloadStopTask", g_stop);
    sym(bgft, "sceBgftServiceIntDownloadUnregisterTask", g_unregister);
    sym(bgft, "sceBgftServiceIntDebugDownloadRegisterPkg", g_register_debug);  // optional
    if (!sym(bgft, "sceBgftServiceDownloadGetProgress", g_progress))
        sym(bgft, "sceBgftServiceIntDownloadGetProgress", g_progress);
    if (!ok) {
        g_problem = "The console's install service is missing functions.";
        return false;
    }
    int rc = g_app_inst_init();
    g_app_inst_ready = rc >= 0;
    if (rc < 0) LOG("installer: sceAppInstUtilInitialize 0x%08x", (unsigned)rc);
    // The service keeps its state in a heap the caller gives it, for the whole session.
    static OrbisBgftInitParams params;
    params.heapSize = 0x100000;
    params.heap = calloc(1, params.heapSize);
    rc = g_bgft_init(&params);
    if (rc < 0 && rc != (int32_t)0x80990001 /* already initialised */) {
        g_problem = "The install service did not start (" + hex(rc) + ").";
        LOG("installer: sceBgftServiceIntInit 0x%08x", (unsigned)rc);
        return false;
    }
    g_ready = true;
    g_problem.clear();
    LOG("installer: ready");
    return true;
}

bool installer_ready() { return g_ready; }
std::string installer_problem() { return g_problem; }

static InstallResult bgft_register(const std::string& source, const Item& item);

bool can_install_from_url() { return g_ready; }

bool query_app_registered(const std::string& title, bool& exists) {
    using ExistsFn = int32_t (*)(const char*, int32_t*);
    static ExistsFn query = nullptr;
    if (!g_app_inst_ready || g_app_inst_module < 0) return false;
    if (!query && !sym(g_app_inst_module, "sceAppInstUtilAppExists", query)) return false;
    int32_t installed = 0;
    if (query(title.c_str(), &installed) < 0) return false;
    exists = installed != 0;
    return true;
}

InstallResult platform_install_local(const std::string& file, const Item& item) {
    // The reliable path while sandboxed: hand the finished package to GoldHEN.
    // (The in-process service needs the sandbox escape, which does not answer on
    // every firmware.)
    if (!g_ready) return place_in_install_folder(file, item);
    return bgft_register(file, item);
}

// LncUtil is a separate import library packed inside SystemService. The
// default module lookup cannot resolve it, and the SystemService wrapper is
// restricted to shell callers. Link LncUtil so the loader selects that library.
InstallResult launch_title(const Item& item) {
    InstallResult result;
    int32_t user = -1;
    const int user_rc = sceUserServiceGetForegroundUser(&user);
    if (user_rc < 0) {
        result.message = "Could not find the active console user (" + hex(user_rc) + ").";
        return result;
    }
    const int init_rc = sceLncUtilInitialize();
    LOG("launch: LncUtil initialise -> 0x%08x", (unsigned)init_rc);
    if (init_rc < 0 && init_rc != (int32_t)0x80940018 /* already initialised */) {
        result.message = "The console's launcher did not initialise (" + hex(init_rc) + ").";
        return result;
    }
    LncAppParam param{};
    param.size = sizeof(param);
    param.user_id = (uint32_t)user;
    param.LaunchAppCheck_flag = (LaunchApp_Flag)launch_check_flags(catalog()->config().skip_game_update_checks);
    LOG("launch: elevated %d, sandbox %d, backend %s, check flags 0x%x", platform::sandbox::lifted(), sceKernelIsInSandbox(), platform::sandbox::method(), (unsigned)param.LaunchAppCheck_flag);
    const int32_t rc = sceLncUtilLaunchApp(item.title_id.c_str(), nullptr, &param);
    LOG("launch: LncUtil %s -> 0x%08x", item.title_id.c_str(), (unsigned)rc);
    if (rc < 0 && rc != (int32_t)0x80940010 && rc != (int32_t)0x80940011 && rc != (int32_t)0x8094000c) {
        if (rc == (int32_t)0x8094000f && !platform::sandbox::lifted()) {
            result.message = "The current HEN did not grant app-launch permissions (" + hex(rc) + "). See the service-helper result in the log.";
            return result;
        }
        result.message = "The console refused to start " + item.name + " (" + hex(rc) + "). Start it from the home screen.";
        return result;
    }
    result.ok = true;
    result.message = "Starting " + item.name + "...";
    return result;
}

namespace {
using UninstallFn = int32_t (*)(const char*);

// An AppInstUtil function, from the module loaded at start or any loaded
// module that exports it, with the service initialised. Empty problem: ready.
UninstallFn app_inst_function(const char* name, std::string& problem) {
    static InitFn initialise = nullptr;
    static bool ready = false;
    const int rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL);
    LOG("app inst: load AppInstUtil -> 0x%08x", (unsigned)rc);
    int module = g_app_inst_module;
    if (module < 0) module = load_system_library("libSceAppInstUtil.sprx");
    if (module < 0) {
        OrbisKernelModule modules[256]{};
        size_t count = 0;
        if (sceKernelGetModuleList(modules, 256, &count) >= 0)
            for (size_t i = 0; i < std::min(count, size_t(256)); ++i) {
                void* symbol = nullptr;
                if (sceKernelDlsym(modules[i], name, &symbol) >= 0 && symbol) {
                    module = (int)modules[i];
                    break;
                }
            }
    }
    UninstallFn fn = nullptr;
    if (module < 0 || (!initialise && !sym(module, "sceAppInstUtilInitialize", initialise)) || !sym(module, name, fn)) {
        problem = "The console's app removal service is unavailable.";
        return nullptr;
    }
    if (!ready) ready = g_app_inst_ready;
    if (!ready) {
        const int init = initialise();
        LOG("app inst: initialise -> 0x%08x", (unsigned)init);
        if (init < 0) {
            problem = "The app removal service did not initialise (" + hex(init) + ").";
            return nullptr;
        }
        ready = true;
    }
    problem.clear();
    return fn;
}
}  // namespace

InstallResult delete_app(const Item& item) {
    refresh_installed(true);
    InstallResult result;
    result.message = app_delete_problem(item);
    if (!result.message.empty()) return result;
    static UninstallFn uninstall = nullptr;
    if (!uninstall) {
        std::string problem;
        uninstall = app_inst_function("sceAppInstUtilAppUnInstall", problem);
        if (!uninstall) {
            result.message = problem + " Delete the app from the home screen.";
            return result;
        }
    }
    const int rc = uninstall(item.title_id.c_str());
    LOG("delete app: %s -> 0x%08x", item.title_id.c_str(), (unsigned)rc);
    if (rc != 0) {
        result.message = "Could not delete " + item.name + " (" + hex(rc) + ").";
        return result;
    }
    app_removal_started(item);
    refresh_installed(true);
    result.ok = true;
    result.message = app_removal_pending(item) ? "Removing " + item.name + " from the console..." : "Deleted " + item.name + ".";
    return result;
}

InstallResult install_from_url(const std::string& url, const Item& item) {
    InstallResult result;
    if (const std::string self = self_install_problem(url, item); !self.empty()) {
        result.message = self;
        return result;
    }
    if (!g_ready && !installer_init()) {
        result.message = "The console cannot install from a link on this firmware. Download the package first.";
        return result;
    }
    return bgft_register(url, item);
}

static InstallResult bgft_register(const std::string& source, const Item& item) {
    InstallResult result;
    const bool remote = lib::starts_with(source, "http://") || lib::starts_with(source, "https://");
    PackageHeader header;
    if (!read_package_header(source, header)) {
        // Not fatal for a URL (the server may refuse range requests); the
        // service reads the package itself.
        LOG("installer: could not read the package header of %s", remote ? "the URL" : source.c_str());
        if (!remote) {
            result.message = "This file is not a PS4 package.";
            return result;
        }
        header.package_type = item.category == Category::DLC ? "PS4AC" : "PS4GD";
    }
    int32_t user = -1;
    sceUserServiceGetForegroundUser(&user);

    // archive.org and others redirect to a mirror: the service is given the final address.
    const std::string url = remote ? (header.final_url.empty() ? source : header.final_url) : bgft::storage_path(source);
    bgft::StorageParameters ex{};
    auto& p = ex.params;
    if (remote) {
        p.userId = user;
        p.entitlementType = 5;
        p.id = header.content_id.c_str();
        p.contentUrl = url.c_str();
        p.contentName = item.name.c_str();
        p.iconPath = item.cover_url.c_str();
        p.playgoScenarioId = "0";
        p.option = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;
        p.packageType = header.package_type.c_str();
        p.packageSubType = "";
        p.packageSize = item.size;
    } else {
        if (g_title_from_pkg) {
            char actual[16]{};
            int32_t is_app = 0;
            const int rc = g_title_from_pkg(url.c_str(), actual, &is_app);
            actual[sizeof(actual) - 1] = '\0';
            LOG("installer: validate local PKG -> 0x%08x, title %s, app %d", (unsigned)rc, actual, is_app);
            if (rc != 0) { result.message = "The console could not validate this local PKG (" + hex(rc) + "). The file was kept."; return result; }
            if (!item.title_id.empty() && item.title_id != actual) {
                result.message = "This PKG belongs to a different title (" + std::string(actual) + "). No install was started.";
                return result;
            }
        }
        LOG("installer: local storage content_url %s", url.c_str());
        ex = bgft::local_parameters(url.c_str(), item.name.c_str(), item.cover_url.c_str());
    }

    int32_t task = -1;
    int32_t rc = g_register_ex(&ex, &task);
    LOG("installer: register %s %s (%s): 0x%08x task %d", remote ? "URL" : "file", item.title_id.c_str(),
        header.package_type.c_str(), (unsigned)rc, task);
    if (rc == (int32_t)0x80990086) {
        result.message = "The console already has a download/install task for " + item.name +
            ". Check Notifications > Downloads: let an active install finish, or delete its failed task before retrying.";
        return result;
    }
    if (rc == (int32_t)0x80990088 || rc == (int32_t)0x80990015) {
        result.message = item.name + " is already installed or being installed.";
        return result;
    }
    if (rc < 0 && !remote && g_register_debug) {
        rc = g_register_debug(&ex, &task);
        LOG("installer: debug register: 0x%08x task %d", (unsigned)rc, task);
    }
    if (rc == (int32_t)0x80990088 || rc == (int32_t)0x80990015) {
        result.message = item.name + " is already installed or being installed.";
        return result;
    }
    if (rc < 0) {
        result.message = "The console refused the package (" + hex(rc) + ").";
        return result;
    }
    rc = g_start(task);
    if (rc < 0) {
        result.message = "The install did not start (" + hex(rc) + ").";
        std::string problem;
        if (!release_install_task(task, true, problem)) {
            result.task_id = task;
            result.message += " " + problem;
        }
        return result;
    }
    result.ok = true;
    result.message = "Installing " + item.name + ": see Notifications \xE2\x80\xBA Downloads";
    result.task_id = task;
    return result;
}

bool release_install_task(int32_t task_id, bool stop, std::string& problem) {
    if (task_id < 0 || !g_unregister) {
        problem = "The console could not release the install task; the PKG is kept.";
        return false;
    }
    if (stop && g_stop) {
        const int rc = g_stop(task_id);
        LOG("installer: stop owned task %d -> 0x%08x", task_id, (unsigned)rc);
    }
    const int rc = g_unregister(task_id);
    LOG("installer: unregister owned task %d -> 0x%08x", task_id, (unsigned)rc);
    if (rc != 0) {
        problem = "The console still has the install task (" + hex(rc) + "); the PKG is kept.";
        return false;
    }
    problem.clear();
    return true;
}

// ---- the Sony update blocker (installer.h) ----
// Signatures read from the console's own libSceBgft (each export only adds
// the library's context and forwards to the download daemon):
//   IntGetTaskInfoValueString(taskId, int key, char* buf, size_t size <= 0x8000, size_t* length)
//   IntGetTaskInfoValueInt(taskId, int key, int* value)
//   IntDownloadFindActiveTask(contentId, subType, int* taskId) and friends
// The FindActive* lookups did not find a Sony update that had failed at once
// (SUPERHOT, 2026-11-28), so the queue is walked by task number instead:
// numbers only grow (433..446 seen on the console), a number exists when its
// progress can be read. Which key holds the source URL is not documented:
// every key is read. A task is stopped only when one value names Sony's
// servers AND one names the title ID of a game installed as an fPKG (a system
// software update also comes from playstation.net and must never be touched).
namespace {
using ValueStringFn = int32_t (*)(int32_t, int32_t, char*, size_t, size_t*);
using ValueIntFn = int32_t (*)(int32_t, int32_t, int32_t*);
constexpr int kInfoKeys = 64;
constexpr int32_t kFirstScanEnd = 8192;  // task numbers looked at once at start
constexpr int32_t kScanBatch = 128;      // per frame while doing that
constexpr int32_t kWindowBelow = 8, kWindowAbove = 64;
// A Sony download starts within seconds of a game being started. The queue is
// looked at this often; each look briefly borrows authority in the background
// service, so the interval is kept long to minimise that exposure while still
// catching an update soon after launch.
constexpr uint64_t kWindowEveryMs = 20000;
// How long the service must watch without trouble before the one-shot latch is
// promoted (so a quick freeze after the first borrow never retries on reboot).
constexpr uint64_t kBlockerProveMs = 120000;

void notify_on_screen(const std::string& text) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof req);
    req.type = NotificationRequest;
    req.targetId = -1;
    req.useIconImageUri = 1;
    snprintf(req.iconUri, sizeof req.iconUri, "%s", "/user/appmeta/FPKP00001/icon0.png");
    snprintf(req.message, sizeof req.message, "%s", text.c_str());
    const int rc = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    LOG("update blocker: notification -> 0x%08x", (unsigned)rc);
}

std::string title_name(const std::string& title_id) {
    const auto cat = catalog();
    if (cat->loaded())
        for (int c = 0; c < (int)Category::Count; ++c)
            for (const auto& item : cat->items((Category)c))
                if (item.title_id == title_id && c != (int)Category::Updates && c != (int)Category::DLC) return item.name;
    return title_id;
}

// Shared with the background service (<data>/service/):
//   fpkg-titles.json  {"CUSA09030": "SUPERHOT", ...} written by the app: the
//                     installed fPKGs with their names (the service runs in
//                     its sandbox, where /user/app may not be readable)
//   blocker.json      {"state": "watching"|"unavailable"|"off", "time": ...}
//                     written by the service while it runs
std::string blocker_file(const char* name) { return lib::join_path(lib::join_path(platform::data_root(), "service"), name); }
constexpr int64_t kBlockerStatusFreshSeconds = 30;

void write_fpkg_titles(const std::map<std::string, std::string>& titles) {
    lib::Json json = lib::Json::object();
    for (const auto& [id, name] : titles) json.set(id, name);
    lib::make_dirs(lib::join_path(platform::data_root(), "service"));
    lib::write_file_atomic(blocker_file("fpkg-titles.json"), json.dump());
}

void read_fpkg_titles(std::map<std::string, std::string>& titles) {
    std::string text;
    if (!lib::read_file(blocker_file("fpkg-titles.json"), text, 1 << 20)) return;
    bool ok = false;
    const lib::Json json = lib::Json::parse(text, &ok);
    if (!ok || !json.is_object()) return;
    for (const auto& [id, name] : json.members()) titles[id] = name.str().empty() ? id : name.str();
}

// Starts the console's download service and checks the queue can actually be
// read from here. Called once sandboxed, and again after borrowing system
// credentials. Returns true when a task number in 0..1023 answers.
bool bgft_queue_usable(int bgft, OrbisBgftInitParams* params, std::string& problem) {
    static BgftInitFn int_init = nullptr, app_init = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        sym(bgft, "sceBgftServiceIntInit", int_init);
        sym(bgft, "sceBgftServiceInit", app_init);
        g_bgft_init = int_init;
    }
    int rc = int_init ? int_init(params) : (int32_t)0x80990050;
    LOG("update blocker (service): sceBgftServiceIntInit -> 0x%08x", (unsigned)rc);
    if (rc < 0 && rc != (int32_t)0x80990001 /* already initialised */ && app_init) {
        rc = app_init(params);
        LOG("update blocker (service): sceBgftServiceInit -> 0x%08x", (unsigned)rc);
    }
    if (rc < 0 && rc != (int32_t)0x80990001) {
        problem = "the download service did not start (" + hex(rc) + ")";
        return false;
    }
    int found = 0, first_rc = 0;
    for (int32_t id = 0; id < 1024; ++id) {
        NativeBgftProgress progress{};
        const int prc = g_progress(id, &progress);
        if (prc == 0) ++found;
        else if (!first_rc) first_rc = prc;
    }
    LOG("update blocker (service): %d task(s) readable in 0..1023 (first refusal 0x%08x)", found, (unsigned)first_rc);
    if (found == 0) {
        problem = "the download queue cannot be read (" + hex(first_rc) + ")";
        return false;
    }
    return true;
}

// ---- the borrow marker ----
// <data>/service/blocker-borrow.mark says "borrowing <time>" while the service
// holds borrowed access and "idle <time>" otherwise, written in place and
// flushed to disk each time (not renamed: a rename can be lost in a power cut,
// so a stale "borrowing" would come back). Found as "borrowing" at start, the
// console stopped (froze or lost power) while the service held access: that is
// logged, counted in blocker-stops, and after kStopsBeforeOff such stops the
// borrow is switched off for good (the latch is set to "failed").
constexpr int kStopsBeforeOff = 2;

void write_mark(const char* state) {
    const std::string path = blocker_file("blocker-borrow.mark");
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return;
    char text[48];
    const int n = snprintf(text, sizeof text, "%-9s %lld\n", state, (long long)time(nullptr));  // fixed length
    if (pwrite(fd, text, n, 0) == n) fsync(fd);
    close(fd);
}

// True when borrowing may go on; false when earlier stops switched it off.
bool check_previous_stop() {
    static int verdict = -1;
    if (verdict >= 0) return verdict == 1;
    verdict = 1;
    std::string mark;
    if (!lib::read_file(blocker_file("blocker-borrow.mark"), mark, 64) || !lib::starts_with(mark, "borrowing")) return true;
    std::string text;
    lib::read_file(blocker_file("blocker-stops"), text, 32);
    const int stops = atoi(text.c_str()) + 1;
    lib::write_file_atomic(blocker_file("blocker-stops"), std::to_string(stops) + "\n");
    write_mark("idle");
    LOG("update blocker (service): the console stopped while this service held borrowed access (%s); stop %d of %d",
        lib::trim(mark).c_str(), stops, kStopsBeforeOff);
    if (stops >= kStopsBeforeOff) {
        lib::write_file_atomic(blocker_file("blocker-auth.state"), "failed\n");
        LOG("update blocker (service): borrowing switched off (delete service/blocker-stops and "
            "service/blocker-auth.state to allow it again)");
        verdict = 0;
    }
    return verdict == 1;
}

bool borrow_marked() {
    write_mark("borrowing");
    if (platform::sandbox::borrow_authority()) return true;
    write_mark("idle");
    return false;
}

void restore_marked() {
    platform::sandbox::restore_authority();
    write_mark("idle");
}

// The background service's own start of the download service: only what the
// blocker uses (reading the queue, stopping and removing a task).
//
// Sandboxed, the console's download service refuses to start for the
// background service (sceBgftServiceIntInit 0x80990050). A full sandbox escape
// gets past this but destabilised the console (a power-off, then a freeze,
// 2026-11-28). Instead the service borrows ONLY authority and stays inside the
// filesystem jail (sandbox::borrow_authority). That is an experiment, so the
// very first attempt is guarded by an on-disk one-shot latch
// (<data>/service/blocker-auth.state) written and flushed BEFORE it: if the
// console does not survive, the next boot sees "attempting" and never retries.
// The latch is promoted to "ok" only after the service has watched for a while
// without trouble. The user re-enables a stuck latch by deleting the file.
bool blocker_service_init(std::string& problem) {
    static int state = 0;  // 0 not tried, 1 ready, -1 failed
    static std::string failed;
    if (g_ready || state == 1) return true;
    if (state < 0) { problem = failed; return false; }
    state = -1;
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_BGFT);
    const int bgft = load_system_library("libSceBgft.sprx");
    if (bgft < 0) {
        failed = problem = "the download service could not be loaded (" + hex(bgft) + ")";
        return false;
    }
    sym(bgft, "sceBgftServiceDownloadStopTask", g_stop);
    sym(bgft, "sceBgftServiceIntDownloadUnregisterTask", g_unregister);
    if (!sym(bgft, "sceBgftServiceDownloadGetProgress", g_progress))
        sym(bgft, "sceBgftServiceIntDownloadGetProgress", g_progress);
    if (!g_progress || !g_unregister) {
        failed = problem = "the download service is missing functions";
        return false;
    }
    static OrbisBgftInitParams params;
    params.heapSize = 0x100000;
    if (!params.heap) params.heap = calloc(1, params.heapSize);

    // Sandboxed only (no privilege change): try it first in case it is allowed.
    if (bgft_queue_usable(bgft, &params, problem)) { state = 1; return true; }

    // Authority-only borrow, once, guarded by the on-disk latch.
    const std::string latch = blocker_file("blocker-auth.state");
    std::string mark;
    lib::read_file(latch, mark, 64);
    mark = lib::trim(mark);
    if (mark == "attempting") {
        failed = problem = "a previous background-access attempt did not finish; not retrying (delete "
                           "service/blocker-auth.state to allow it again)";
        LOG("update blocker (service): %s", problem.c_str());
        return false;
    }
    if (mark == "failed") {
        failed = problem = "borrowing background access is switched off on this console (not retrying)";
        return false;
    }
    LOG("update blocker (service): queue refused sandboxed; trying a one-shot authority borrow");
    lib::write_file_atomic(latch, "attempting\n");  // flushed before the risky step
    if (!borrow_marked()) {
        lib::write_file_atomic(latch, "ready\n");  // no privilege change happened; retryable
        failed = problem = "the background service cannot borrow authority on this console";
        LOG("update blocker (service): %s", problem.c_str());
        return false;
    }
    const bool usable = bgft_queue_usable(bgft, &params, problem);
    restore_marked();
    if (usable) {
        // Keep the latch at "attempting" until the survival watchdog promotes
        // it (update_blocker_tick): a quick freeze after this must not retry.
        LOG("update blocker (service): queue readable after borrowing authority");
        state = 1;
        return true;
    }
    lib::write_file_atomic(latch, "failed\n");
    failed = problem = "borrowing authority did not make the queue readable (" + problem + ")";
    return false;
}

void write_blocker_status(const char* state) {
    lib::Json json = lib::Json::object();
    json.set("state", state);
    json.set("time", (int64_t)time(nullptr));
    lib::write_file_atomic(blocker_file("blocker.json"), json.dump());
}
}  // namespace

bool service_blocker_active() {
    std::string text;
    if (!lib::read_file(blocker_file("blocker.json"), text, 4096)) return false;
    bool ok = false;
    const lib::Json json = lib::Json::parse(text, &ok);
    if (!ok) return false;
    const int64_t age = (int64_t)time(nullptr) - json["time"].integer(0);
    return json["state"].str() == "watching" && age >= 0 && age <= kBlockerStatusFreshSeconds;
}

std::vector<std::string> update_blocker_tick(bool from_service, bool settled) {
    std::vector<std::string> out;
    const bool enabled = catalog()->config().block_sony_updates;

    // Installed fPKGs by title ID, with names. The app scans /user/app and
    // writes the list for the service; the service reads that list (checked
    // every 10 s) and adds what it can scan itself.
    static uint64_t scanned_revision = UINT64_MAX;
    static std::map<std::string, std::string> fakes;
    static std::string titles_text;
    static uint64_t titles_checked = 0;
    static bool names_resolved = false;  // app: the list was written with the catalogue loaded
    const uint64_t now_tick = platform::now_ms();
    if (from_service) refresh_installed();
    bool changed = false;
    // The app resolves fPKG names from the catalogue, which loads shortly after
    // start: rescan when installs change, and once more when the catalogue is
    // ready, so the service's list has real names and not title IDs.
    const bool app_names_now_available = !from_service && !names_resolved && catalog()->loaded();
    if (scanned_revision != installed_revision() || app_names_now_available) {
        scanned_revision = installed_revision();
        std::map<std::string, std::string> scanned;
        for (const auto& id : installed_app_ids()) {
            std::string content_id;
            bool fake = false;
            if (installed_app_package(id, content_id, fake) && fake) scanned[id] = title_name(id);
        }
        if (!from_service) {
            fakes = scanned;
            write_fpkg_titles(fakes);
            if (catalog()->loaded()) names_resolved = true;
        } else {
            for (const auto& [id, name] : scanned) fakes.emplace(id, name);
        }
        changed = true;
    }
    if (from_service && now_tick - titles_checked >= 10000) {
        titles_checked = now_tick;
        std::string text;
        if (lib::read_file(blocker_file("fpkg-titles.json"), text, 1 << 20) && text != titles_text) {
            titles_text = text;
            std::map<std::string, std::string> listed;
            read_fpkg_titles(listed);
            for (const auto& [id, name] : listed) fakes[id] = name;
            changed = true;
        }
    }
    if (changed) LOG("update blocker: %zu installed fPKG titles", fakes.size());

    // The service reports what it is doing; the app watches only when the
    // service does not (not running, an older build, or Bgft unavailable).
    static uint64_t status_written = 0;
    static const char* last_state = "";
    if (from_service) {
        // Not while the console goes into rest mode or has just woken: no
        // borrow, no status (the app watches the queue itself if it is open).
        static bool was_settled = true;
        if (settled != was_settled) {
            LOG("update blocker (service): %s", settled ? "console settled; watching again" : "paused (rest mode or just woken)");
            was_settled = settled;
        }
        if (!settled) return out;
        if (!check_previous_stop()) {
            if (strcmp(last_state, "unavailable") != 0) {
                last_state = "unavailable";
                write_blocker_status(last_state);
            }
            return out;
        }
        std::string problem;
        const bool watching = blocker_service_init(problem);
        const char* state = !enabled ? "off" : watching ? "watching" : "unavailable";
        if (strcmp(state, last_state) != 0 || now_tick - status_written >= 10000) {
            if (strcmp(state, last_state) != 0)
                LOG("update blocker (service): %s%s", state, watching || problem.empty() ? "" : (" - " + problem).c_str());
            last_state = state;
            status_written = now_tick;
            write_blocker_status(state);
        }
        if (strcmp(state, "watching") != 0) return out;
        // Survival watchdog: once the service has watched this long without
        // trouble, the one-shot latch is promoted from "attempting" to "ok" so
        // later boots may borrow again. A freeze before this leaves "attempting".
        static uint64_t watching_since = 0;
        if (watching_since == 0) watching_since = now_tick;
        static bool promoted = false;
        if (!promoted && now_tick - watching_since >= kBlockerProveMs) {
            promoted = true;
            const std::string latch = blocker_file("blocker-auth.state");
            std::string mark;
            lib::read_file(latch, mark, 64);
            if (lib::trim(mark) == "attempting") {
                lib::write_file_atomic(latch, "ok\n");
                LOG("update blocker (service): authority borrow proved stable; latch set ok");
            }
        }
    } else {
        if (!enabled || !g_ready) return out;
        static uint64_t service_checked = 0;
        static int service_watching = -1;  // unknown until first checked
        if (service_watching < 0 || now_tick - service_checked >= 5000) {
            service_checked = now_tick;
            const int watching = service_blocker_active() ? 1 : 0;
            if (watching != service_watching)
                LOG("update blocker: %s", watching ? "the background service is watching the queue" : "watching the queue from the app");
            service_watching = watching;
        }
        if (service_watching) return out;
    }

    static bool resolved = false, usable = false;
    static ValueStringFn value_string = nullptr;
    static ValueIntFn value_int = nullptr;
    if (!resolved) {
        resolved = true;
        const int bgft = load_system_library("libSceBgft.sprx");
        if (bgft >= 0) {
            sym(bgft, "sceBgftServiceIntGetTaskInfoValueString", value_string);
            sym(bgft, "sceBgftServiceIntGetTaskInfoValueInt", value_int);
        }
        usable = value_string && g_unregister && g_progress;
        LOG("update blocker: %s", usable ? "watching the download queue" : "unavailable (functions missing)");
    }
    if (!usable) return out;

    // Which task numbers to look at now: the first scan a batch per frame,
    // then every kWindowEveryMs the numbers around and above the highest seen.
    static int32_t scan_next = 0, highest = -1;
    static uint64_t next_window = 0;
    int32_t from = 0, to = 0;
    const uint64_t now = platform::now_ms();
    if (scan_next < kFirstScanEnd) {
        from = scan_next;
        to = std::min(kFirstScanEnd, scan_next + kScanBatch);
        scan_next = to;
        if (scan_next >= kFirstScanEnd) LOG("update blocker: queue scanned, highest task %d", highest);
    } else if (now >= next_window) {
        next_window = now + kWindowEveryMs;
        from = std::max(0, highest - kWindowBelow);
        to = std::max(highest, 0) + kWindowAbove;
    } else {
        return out;
    }

    // The background service borrows authority (staying in the sandbox) for
    // just this scan and hands it straight back. The app is already privileged.
    struct ServiceAccess {
        bool held = false;
        explicit ServiceAccess(bool on) { if (on) held = borrow_marked(); }
        ~ServiceAccess() { if (held) restore_marked(); }
    } access(from_service);
    if (from_service && !access.held) return out;  // could not borrow this pass

    static std::map<int32_t, bool> verdicts;  // examined tasks; true: Sony's, stopped
    static char buf[0x2000];
    for (int32_t id = from; id < to; ++id) {
        if (verdicts.count(id) || installer_owns_task(id)) continue;
        NativeBgftProgress progress{};
        if (g_progress(id, &progress) != 0) continue;  // no such task
        highest = std::max(highest, id);
        // What the task says about itself, logged the first time it is seen.
        bool sony = false;
        std::string source, title;
        std::string summary;
        for (int key = 0; key < kInfoKeys; ++key) {
            size_t length = 0;
            buf[0] = 0;
            if (value_string(id, key, buf, sizeof buf - 1, &length) < 0) continue;
            buf[std::min(length, sizeof buf - 1)] = 0;
            const std::string value(buf);
            if (value.empty()) continue;
            summary += " s" + std::to_string(key) + "=" + value;
            if (is_sony_source(value)) { sony = true; source = value; }
            for (const auto& fake : fakes)
                if (title.empty() && value.find(fake.first) != std::string::npos) title = fake.first;
        }
        if (value_int)
            for (int key = 0; key < kInfoKeys; ++key) {
                int32_t v = 0;
                if (value_int(id, key, &v) >= 0 && v != 0) summary += " i" + std::to_string(key) + "=" + std::to_string(v);
            }
        LOG("update blocker: task %d (bits 0x%x, error 0x%08x):%s", id, progress.bits, (unsigned)progress.error, summary.c_str());
        // Never touch a PS4 system software update (PS4UPDATE.PUP from
        // *.update.playstation.net): only game update packages are blocked.
        const bool system_update = source.find("update.playstation.net") != std::string::npos ||
                                   source.find("PS4UPDATE.PUP") != std::string::npos;
        if (system_update || !sony || title.empty()) {
            verdicts[id] = false;
            LOG("update blocker: task %d left alone (%s)", id,
                system_update ? "system software update" : !sony ? "not from PlayStation Network"
                                                                 : "not for a game installed as an fPKG");
            continue;
        }
        std::string problem;
        const bool stopped = release_install_task(id, true, problem);
        LOG("update blocker: %s task %d from %s -> %s", title.c_str(), id, source.c_str(), stopped ? "removed" : problem.c_str());
        verdicts[id] = true;
        const std::string name = fakes[title].empty() ? title : fakes[title];
        const std::string message = stopped
            ? "Blocked a PlayStation Network update for " + name + " (it does not run on this package)."
            : "Could not remove a PlayStation Network update for " + name + ". Remove it in Notifications > Downloads.";
        if (!from_service) notify_on_screen(message);  // the service shows its own, held during rest mode
        out.push_back(message);
    }
    return out;
}

InstallTaskProgress query_install_task(int32_t task_id) {
    if (!g_progress) return {InstallTaskState::Unknown, "Install progress is unavailable; the PKG is kept."};
    NativeBgftProgress progress{};
    const int rc = g_progress(task_id, &progress);
    if (rc != 0) return {InstallTaskState::Unknown, "Install status is unavailable (" + hex(rc) + "); the PKG is kept."};
    if (progress.error != 0) {
        LOG("installer: task %d progress bits 0x%x error 0x%08x, copied %llu/%llu, local copy %d", task_id,
            progress.bits, (unsigned)progress.error, (unsigned long long)progress.transferred,
            (unsigned long long)progress.length, progress.local_copy_percent);
        return {InstallTaskState::Failed, "Install failed (" + hex(progress.error) + "); the PKG is kept."};
    }
    const bool copied = progress.length > 0 && progress.transferred >= progress.length &&
        (progress.length_total == 0 || progress.transferred_total >= progress.length_total);
    const bool complete = copied && progress.local_copy_percent == 100;
    const uint64_t total = progress.length_total ? progress.length_total : progress.length;
    const uint64_t done = progress.length_total ? progress.transferred_total : progress.transferred;
    const int percent = total ? (int)std::min(complete ? 100.0 : 99.0, 100.0 * (double)std::min(done, total) / (double)total) : -1;
    return {complete ? InstallTaskState::Complete : InstallTaskState::Pending,
        complete ? "" : total && done >= total ? "Finalizing installation..." : total ? "Installing" : "Preparing installation...",
        complete ? 100 : percent, done, total};
}

}  // namespace store
