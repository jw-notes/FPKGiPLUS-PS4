// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Leaving the sandbox with GoldHEN's own escape.
//
// GoldHEN 2.3 and later answer syscall 500 ("SDK commands", the interface of
// the GoldHEN Plugins SDK, MIT, vendored in third_party/goldhen_sdk). Command 2,
// sys_sdk_jailbreak, saves the process's credentials (uid, ruid, rgid, groups,
// auth id, capabilities), prison and root/jail/current directories into a
// jailbreak_backup and replaces them with the system's; command 3,
// sys_sdk_unjailbreak, writes a backup back. GoldHEN's kernel side is not
// public: what it changes is known from that structure only.
//
// The jailbreak makes the process root, and FPKGi+ stays root: /user/app (to
// tell what is installed) is readable by root only, and the console's install
// service needs the raised auth id and capabilities.
#include "platform/sandbox_ps4.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <orbis/libkernel.h>

#include <vector>

#include "core/log.h"

extern "C" {
#include "goldhen_sdk/include/GoldHEN.h"
#include "itemzflow_jbc/include/multi-jb.h"
int orbis_syscall(int num, ...);  // goldhen_sdk/source/Syscall.c
}

namespace {

// The console's kernel is FreeBSD: its SIGSYS is 12 (the toolchain's
// <signal.h> has Linux numbers, where SIGSYS is 31), and libkernel's sigaction
// takes FreeBSD's structure. Called directly, not through musl's signal().
constexpr int kSigSys = 12;
constexpr int kSysSetgid = 181;
struct FreeBsdSigaction {
    void (*handler)(int);
    int flags;
    uint32_t mask[4];
};
extern "C" int fbsd_sigaction(int sig, const FreeBsdSigaction* act, FreeBsdSigaction* old) __asm__("sigaction");

// A syscall the kernel does not have raises SIGSYS, which ends the process:
// ignored while GoldHEN is asked (it then fails with ENOSYS instead).
class IgnoreSigSys {
public:
    IgnoreSigSys() {
        FreeBsdSigaction ignore{};
        ignore.handler = (void (*)(int))1;  // SIG_IGN
        saved_ = fbsd_sigaction(kSigSys, &ignore, &old_) == 0;
    }
    ~IgnoreSigSys() {
        if (saved_) fbsd_sigaction(kSigSys, &old_, nullptr);
    }

private:
    FreeBsdSigaction old_{};
    bool saved_ = false;
};

jailbreak_backup g_backup{};
bool g_lifted = false;
bool g_jbc_active = false;
jbc_cred g_jbc_backup{};
using JbcGet = int (*)(jbc_cred*);
using JbcSet = int (*)(const jbc_cred*);
using JbcLift = int (*)(jbc_cred*);
JbcGet g_jbc_get = nullptr;
JbcSet g_jbc_set = nullptr;
JbcLift g_jbc_lift = nullptr;
using JbcThread = uintptr_t (*)();
JbcThread g_jbc_thread = nullptr;
uid_t g_uid = 0;
gid_t g_gid = 0;
std::vector<gid_t> g_groups;

void log_identity(const char* when) {
    LOG("usb (%s): uid %u euid %u gid %u egid %u, in sandbox %d", when, (unsigned)getuid(), (unsigned)geteuid(),
        (unsigned)getgid(), (unsigned)getegid(), sceKernelIsInSandbox());
}

// Back to the app's own groups, gid and uid (in that order: each needs root).
[[maybe_unused]] bool drop_root() {
    if (geteuid() != 0) return true;
    if (g_uid == 0) {
        LOG("usb: the app itself runs as uid 0; nothing to drop");
        return true;
    }
    bool ok = true;
    if (!g_groups.empty() && setgroups(g_groups.size(), g_groups.data()) != 0) {
        LOG("usb: setgroups failed (errno %d)", errno);
        ok = false;
    }
    {
        IgnoreSigSys guard;
        if (orbis_syscall(kSysSetgid, (int)g_gid) != 0) {
            LOG("usb: setgid(%u) failed (errno %d)", (unsigned)g_gid, errno);
            ok = false;
        }
    }
    if (setuid(g_uid) != 0) {
        LOG("usb: setuid(%u) failed (errno %d)", (unsigned)g_uid, errno);
        ok = false;
    }
    log_identity(ok ? "root dropped" : "root NOT fully dropped");
    return ok;
}

bool itemzflow_jailbreak() {
    IgnoreSigSys guard;
    if (!g_jbc_get) {
        const int module = sceKernelLoadStartModule("/app0/sce_module/fpkgi-jbc.prx", 0, nullptr, 0, nullptr, nullptr);
        if (module < 0) {
            LOG("services: Itemzflow helper could not load (0x%08x)", (unsigned)module);
            return false;
        }
        void* get = nullptr; void* set = nullptr; void* lift = nullptr; void* thread = nullptr;
        sceKernelDlsym(module, "jbc_get_cred", &get);
        sceKernelDlsym(module, "jbc_set_cred", &set);
        sceKernelDlsym(module, "jbc_jailbreak_cred", &lift);
        sceKernelDlsym(module, "jbc_krw_get_td", &thread);
        if (!get || !set || !lift || !thread) return false;
        g_jbc_get = (JbcGet)get; g_jbc_set = (JbcSet)set;
        g_jbc_lift = (JbcLift)lift; g_jbc_thread = (JbcThread)thread;
    }
    // Probe before walking kernel structures: unsupported syscall 11 must
    // leave the app running with its original credentials.
    const uintptr_t td = g_jbc_thread();
    if (td < 0xffff800000000000ull || td >= 0xffffffff00000000ull) {
        LOG("services: Itemzflow helper's kernel-call interface is unavailable");
        return false;
    }
    if (g_jbc_get(&g_jbc_backup) != 0) {
        LOG("services: Itemzflow helper could not save app credentials");
        return false;
    }
    jbc_cred system = g_jbc_backup;
    if (g_jbc_lift(&system) != 0) return false;
    const int rc = g_jbc_set(&system);
    LOG("services: Itemzflow credentials -> %d, auth %016llx, caps %016llx", rc,
        (unsigned long long)system.sceProcType, (unsigned long long)system.sceProcCap);
    if (rc != 0 || sceKernelIsInSandbox() != 0) {
        g_jbc_set(&g_jbc_backup);
        return false;
    }
    g_jbc_active = true;
    g_lifted = true;
    log_identity("Itemzflow helper");
    return true;
}

bool jailbreak() {
    if (g_jbc_active) return itemzflow_jailbreak();
    int rc, err;
    {
        IgnoreSigSys guard;
        errno = 0;
        rc = sys_sdk_jailbreak(&g_backup);
        err = errno;
    }
    if (rc != 0) {
        LOG("usb: GoldHEN jailbreak failed (rc %d, errno %d)", rc, err);
        return false;
    }
    g_lifted = true;
    return true;
}

}  // namespace

namespace platform::sandbox {

const char* method() { return g_jbc_active ? "Itemzflow libjbc helper" : "GoldHEN SDK"; }
bool lifted() { return g_lifted; }

bool leave() {
    if (g_lifted) return true;
    uint32_t version;
    int err;
    {
        IgnoreSigSys guard;
        errno = 0;
        version = sys_sdk_version();
        err = errno;
    }
    if (version == 0 || version == 0xFFFFFFFFu) {
        LOG("usb: GoldHEN's SDK does not answer (syscall 500, errno %d): GoldHEN 2.3 or later is needed for USB "
            "drives",
            err);
        LOG("services: trying Itemzflow's credential helper");
        return itemzflow_jailbreak();
    }
    LOG("usb: GoldHEN SDK %u.%02x", (unsigned)(version >> 8), (unsigned)(version & 0xFF));

    g_uid = getuid();
    g_gid = getgid();
    const int count = getgroups(0, nullptr);
    if (count > 0) {
        g_groups.resize(count);
        if (getgroups(count, g_groups.data()) != count) g_groups.clear();
    }
    log_identity("before");

    if (!jailbreak()) return itemzflow_jailbreak();
    LOG("usb: GoldHEN jailbreak done; saved uid %u ruid %u rgid %u groups %u auth id %016llx caps %016llx %016llx",
        g_backup.cr_uid, g_backup.cr_ruid, g_backup.cr_rgid, g_backup.cr_groups,
        (unsigned long long)g_backup.cr_paid, (unsigned long long)g_backup.cr_caps[0],
        (unsigned long long)g_backup.cr_caps[1]);
    log_identity("jailbroken");
    // Stays root: /user/app (what is installed) and the install service need it.
    const bool out = sceKernelIsInSandbox() == 0;
    LOG("usb: leaving the sandbox with %s %s", method(), out ? "succeeded" : "failed");
    return out;
}

void restore() {
    if (!g_lifted) return;
    int rc;
    {
        IgnoreSigSys guard;
        rc = g_jbc_active ? g_jbc_set(&g_jbc_backup) : sys_sdk_unjailbreak(&g_backup);
    }
    g_lifted = false;
    LOG("services: restoring app credentials through %s, rc %d", method(), rc);
}

void with_app_credentials(const std::function<void()>& fn) {
    if (!g_lifted) {
        fn();
        return;
    }
    restore();
    fn();
    // The backup now holds the app's credentials again.
    jailbreak();
}

}  // namespace platform::sandbox
