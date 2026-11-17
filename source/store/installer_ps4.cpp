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
#include <algorithm>

#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <orbis/_types/bgft.h>

#include "core/log.h"
#include "core/text.h"
#include "platform/sandbox_ps4.h"
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
    const int inst = load("/system/common/lib/libSceAppInstUtil.sprx");
    g_app_inst_module = inst;
    const int bgft = load("/system/common/lib/libSceBgft.sprx");
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

InstallResult delete_app(const Item& item) {
    refresh_installed(true);
    InstallResult result;
    result.message = app_delete_problem(item);
    if (!result.message.empty()) return result;
    using UninstallFn = int32_t (*)(const char*);
    static UninstallFn uninstall = nullptr;
    static InitFn initialise = nullptr;
    static bool ready = false;
    if (!uninstall || !initialise) {
        const int rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL);
        LOG("delete app: load AppInstUtil -> 0x%08x", (unsigned)rc);
        int module = g_app_inst_module;
        if (module < 0) module = load("/system/common/lib/libSceAppInstUtil.sprx");
        if (module < 0) {
            OrbisKernelModule modules[256]{};
            size_t count = 0;
            if (sceKernelGetModuleList(modules, 256, &count) >= 0)
                for (size_t i = 0; i < std::min(count, size_t(256)); ++i) {
                    void* symbol = nullptr;
                    if (sceKernelDlsym(modules[i], "sceAppInstUtilAppUnInstall", &symbol) >= 0 && symbol) {
                        module = (int)modules[i];
                        break;
                    }
                }
        }
        if (module < 0 || !sym(module, "sceAppInstUtilInitialize", initialise) ||
            !sym(module, "sceAppInstUtilAppUnInstall", uninstall)) {
            result.message = "The console's app removal service is unavailable. Delete the app from the home screen.";
            return result;
        }
        ready = g_app_inst_ready;
    }
    if (!ready) {
        const int rc = initialise();
        LOG("delete app: initialise -> 0x%08x", (unsigned)rc);
        if (rc < 0) {
            result.message = "The app removal service did not initialise (" + hex(rc) + ").";
            return result;
        }
        ready = true;
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
