// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The parts of the installer that are the same on the console and the host;
// the install service itself is in installer_ps4.cpp / installer_host.cpp.
#include "store/installer.h"
#include "store/homebrew_updates.h"
#include "store/downloads.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <set>
#include <map>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "core/usb_ftp.h"
#include "net/http.h"
#include "platform/platform.h"

namespace store {

namespace {

std::set<std::string> g_apps, g_patches, g_addcont;
uint64_t g_installed_revision = 0;
std::set<std::string> g_removing;
bool g_scanned = false;

// Adds the title folders in `dir` to `out`. Inside the sandbox /user cannot be
// opened: the folders are then listed through GoldHEN's FTP server, which runs
// outside it (the same way USB drives are reached, core/usb_ftp.h).
void scan(const std::string& dir, std::set<std::string>& out, const char* package = nullptr) {
    std::vector<lib::DirEntry> entries;
    if ((!lib::list_dir(dir, entries, false) || entries.empty()) && lib::usb_ftp::enabled()) {
        entries.clear();
        lib::usb_ftp::list(dir, entries);
    }
    for (const auto& e : entries) {
        if (!e.is_dir || e.name.size() != 9 || e.name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos) continue;
        if (package) {
            const std::string path = lib::join_path(lib::join_path(dir, e.name), package);
            FILE* file = lib::open_file(path, "rb");
            if (!file && lib::usb_ftp::enabled()) file = lib::usb_ftp::open_read(path);
            if (!file) continue;
            unsigned char magic[4]{};
            const bool valid = fread(magic, 1, 4, file) == 4 && memcmp(magic, "\x7f" "CNT", 4) == 0;
            fclose(file);
            if (!valid) continue;
            if (strcmp(package, "app.pkg") == 0) {
                bool registered = false;
                if (query_app_registered(e.name, registered) && !registered) continue;
            }
        }
        out.insert(e.name);
    }
}
uint64_t g_scanned_at = 0;

}  // namespace

std::string system_root() {
#ifdef LIB_HOST
    return lib::join_path(platform::data_root(), "system");
#else
    return "/user";
#endif
}

void refresh_installed(bool force) {
    // Poll while the UI remains open, as well as when screens resume.
    const uint64_t now = platform::now_ms();
    if (g_scanned && !force && now - g_scanned_at < 4000) return;
    g_scanned_at = now;
    const auto old_apps = g_apps, old_patches = g_patches, old_addcont = g_addcont;
    g_apps.clear();
    g_patches.clear();
    g_addcont.clear();
    // The console's own storage, then an extended-storage drive's.
    for (const std::string& root : {system_root(), std::string("/mnt/ext0/user"), std::string("/mnt/ext1/user")}) {
        scan(lib::join_path(root, "app"), g_apps, "app.pkg");
        scan(lib::join_path(root, "patch"), g_patches, "patch.pkg");
        scan(lib::join_path(root, "addcont"), g_addcont);
#ifdef LIB_HOST
        break;
#endif
    }
    for (auto it = g_removing.begin(); it != g_removing.end();) {
        if (!g_apps.count(*it)) it = g_removing.erase(it);
        else ++it;
    }
    if (!g_scanned) LOG("installed: %zu apps, %zu updates, %zu add-on folders", g_apps.size(), g_patches.size(),
                        g_addcont.size());
    g_scanned = true;
    if (g_apps != old_apps || g_patches != old_patches || g_addcont != old_addcont) {
        ++g_installed_revision;
        platform::request_redraw();
    }
}

uint64_t installed_revision() { return g_installed_revision; }

std::vector<std::string> installed_app_ids() {
    if (!g_scanned) refresh_installed();
    std::vector<std::string> out;
    for (const auto& id : g_apps)
        if (!g_removing.count(id)) out.push_back(id);
    return out;
}

bool package_header_is_fake(const std::string& header) {
    if (header.size() < 8 || header.compare(0, 4, "\x7f" "CNT") != 0) return false;
    return ((unsigned char)header[4] & 0x80) == 0;
}

bool installed_app_package(const std::string& title_id, std::string& content_id, bool& fake) {
    if (title_id.size() != 9 || title_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
        return false;
    for (const std::string& root : {system_root(), std::string("/mnt/ext0/user"), std::string("/mnt/ext1/user")}) {
        const std::string path = lib::join_path(root, "app/" + title_id + "/app.pkg");
        FILE* file = lib::open_file(path, "rb");
        if (!file && lib::usb_ftp::enabled()) file = lib::usb_ftp::open_read(path);
        if (!file) {
#ifdef LIB_HOST
            break;
#endif
            continue;
        }
        std::string header(0x80, '\0');
        const bool ok = fread(&header[0], 1, header.size(), file) == header.size();
        fclose(file);
        if (!ok || header.compare(0, 4, "\x7f" "CNT") != 0) return false;
        content_id = std::string(header.data() + 0x40, strnlen(header.data() + 0x40, 0x24));
        fake = package_header_is_fake(header);
        return !content_id.empty();
    }
    return false;
}

bool is_sony_source(const std::string& value) {
    const std::string v = lib::lower(value);
    return v.find("playstation.net") != std::string::npos || v.find("playstation.com") != std::string::npos;
}

bool is_installed(const Item& item) {
    if (!g_scanned) refresh_installed();
    if (g_removing.count(item.title_id)) return false;
    switch (item.category) {
        case Category::Updates: return g_patches.count(item.title_id) > 0;
        case Category::DLC: return g_addcont.count(item.title_id) > 0;
        case Category::Themes: return false;  // themes have no folder of their own
        default: return g_apps.count(item.title_id) > 0;
    }
}

bool app_removal_pending(const Item& item) { return g_removing.count(item.title_id) != 0; }
void app_removal_started(const Item& item) {
    g_removing.insert(item.title_id);
    platform::request_redraw();
}

std::string app_delete_problem(const Item& item) {
    if (item.title_id.size() != 9 || item.title_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
        return "This app has an invalid title ID.";
    if (item.title_id == "FPKP00001" ||
        (lib::starts_with(item.title_id, "NPXS") && item.title_id != "NPXS39041"))
        return "This title cannot be removed from FPKGi+.";
    if (item.category == Category::Updates || item.category == Category::DLC || item.category == Category::Themes)
        return "Select the installed app itself to remove it.";
    if (!is_installed(item)) return "This app is not installed.";
    return "";
}

std::string installed_app_sfo(const Item& item) {
    if (item.title_id.size() != 9 || item.title_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
        return "";
    // appmeta often contains artwork only. Read the actual installed package's
    // public SFO entry, preferring an installed patch to the base app.
    for (const char* kind : {"patch", "app"}) {
        for (const std::string& root : {system_root(), std::string("/mnt/ext0/user"), std::string("/mnt/ext1/user")}) {
            const std::string path = lib::join_path(root, std::string(kind) + "/" + item.title_id + "/" + kind + ".pkg");
            FILE* file = lib::open_file(path, "rb");
            if (!file && lib::usb_ftp::enabled()) file = lib::usb_ftp::open_read(path);
            if (file) {
                std::string bytes;
                const bool ok = read_package_entry([&](uint64_t at, uint64_t length, std::string& out) {
                    if (fseeko(file, (off_t)at, SEEK_SET) != 0) return false;
                    out.resize((size_t)length);
                    return fread(&out[0], 1, (size_t)length, file) == length;
                }, 0x1000, bytes);
                fclose(file);
                if (ok && sfo_string(bytes, "TITLE_ID") == item.title_id && !sfo_string(bytes, "APP_VER").empty()) return bytes;
                // An existing but unreadable patch must not be mistaken for
                // an older base version and trigger a downgrade.
                LOG("installed version: could not verify %s", path.c_str());
                return "";
            }
#ifdef LIB_HOST
            break;
#endif
        }
    }
    // Compatibility with consoles/tools that export a standalone SFO here.
    const std::string path = lib::join_path(lib::join_path(system_root(), "appmeta"), item.title_id) + "/param.sfo";
    std::string bytes;
    if (!lib::read_file(path, bytes, 1 << 20) && lib::usb_ftp::enabled())
        lib::usb_ftp::read_all(path, bytes, 1 << 20);
    return bytes;
}

// SkipLaunchCheck (1) and SkipSystemUpdateCheck (2) control launcher checks;
// neither blocks the shell's independent retail-patch download service.
int launch_check_flags(bool skip_launcher_checks) { return skip_launcher_checks ? 3 : 2; }

namespace {
lib::Json g_pending = lib::Json::array();
std::string g_pending_path;
uint64_t g_polled_at = 0;
uint64_t g_verify_timeout = kInstallVerifyTimeoutMs;
std::map<std::string, InstallTaskProgress> g_install_progress;
// How long an install has waited for the console (to register the version, or
// to let go of the task), by PKG path. Only time the app was running counts:
// the wait survives a restart through the record's "waited_ms", and a long
// gap between two looks (the app was suspended) adds a few seconds, not hours.
struct Wait {
    uint64_t ms = 0, seen = 0, checked = 0;
    bool started = false;
};
std::map<std::string, Wait> g_waits;
constexpr uint64_t kSlowCheckMs = 15000;  // how often a stalled install is looked at again

void update_install_progress(const std::string& path, const InstallTaskProgress& progress) {
    const auto previous = g_install_progress.find(path);
    if (previous == g_install_progress.end() || previous->second.state != progress.state ||
        previous->second.percent != progress.percent || previous->second.done != progress.done ||
        previous->second.total != progress.total || previous->second.message != progress.message ||
        previous->second.attention != progress.attention) {
        g_install_progress[path] = progress;
        platform::request_redraw();
    }
}

std::string wait_text() {
    const uint64_t seconds = g_verify_timeout / 1000;
    return seconds >= 120 ? std::to_string(seconds / 60) + " minutes" : std::to_string(seconds) + " seconds";
}

// Why an install is not shown as installed, and what to do about it.
std::string stall_message(const lib::Json& record, bool copied) {
    const std::string name = record["name"].str(), version = record["version"].str();
    std::string text = name + ": ";
    if (copied)
        text += "copying finished, but the console has not registered " +
                (version.empty() ? std::string("this package") : "version " + version) + " after " + wait_text() + ".";
    else
        text += "the console has not reported on this install for " + wait_text() + ", so it cannot be confirmed" +
                (version.empty() ? "." : " as version " + version + ".");
    return text + " It is not marked as installed and the PKG is kept. Check Notifications > Downloads on the console, "
                  "then choose Check again, or Stop waiting to release the task and retry Install.";
}

std::string release_stall_message(const lib::Json& record, const std::string& problem) {
    return record["name"].str() + (record["version"].str().empty() ? ": install copy finished" : " is installed and verified") +
           ", but the console has not released its install task. " + (problem.empty() ? "" : problem + " ") +
           "The PKG is kept. Remove the finished entry from Notifications > Downloads, then choose Check again.";
}

// What a record says before the console has been asked again (after a restart).
InstallTaskProgress recorded_progress(const lib::Json& record) {
    InstallTaskProgress progress{InstallTaskState::Pending, "Preparing installation..."};
    if (record["stalled"].boolean()) {
        progress.message = stall_message(record, true);
        progress.percent = 99;
        progress.attention = InstallAttention::VerificationStalled;
    } else if (record["release_stalled"].boolean()) {
        progress.message = release_stall_message(record, "");
        progress.percent = 99;
        progress.attention = InstallAttention::ReleaseStalled;
    } else if (record["failed"].boolean() || record["start_failed"].boolean()) {
        progress.state = InstallTaskState::Failed;
        progress.message = "Download complete; installation failed. " + record["failure"].str("The install did not start.") +
                           " Downloaded PKG kept. The console still holds the failed task.";
        progress.attention = InstallAttention::FailedTaskHeld;
    }
    return progress;
}

void load_pending() {
    const std::string path = lib::join_path(platform::data_root(), "pending-installs.json");
    if (path == g_pending_path) return;
    g_pending_path = path;
    g_install_progress.clear();
    g_waits.clear();
    std::string bytes; bool ok = false;
    g_pending = lib::read_file(path, bytes, 1 << 20) ? lib::Json::parse(bytes, &ok) : lib::Json();
    if (!ok || !g_pending.is_array()) g_pending = lib::Json::array();
    for (const auto& pending : g_pending.items())
        g_install_progress[pending["path"].str()] = recorded_progress(pending);
}
void save_pending() { lib::write_file_atomic(g_pending_path, g_pending.dump()); }

// Adds the time since this install was last looked at and notes it in the
// record (in coarse steps, so the file is not rewritten at every poll).
uint64_t waited(lib::Json& record) {
    Wait& wait = g_waits[record["path"].str()];
    const uint64_t now = platform::now_ms();
    if (!wait.started) {
        wait.started = true;
        const int64_t before = record["waited_ms"].integer();
        wait.ms = before > 0 ? (uint64_t)before : 0;
    } else {
        wait.ms += std::min<uint64_t>(now - wait.seen, 5000);
    }
    wait.seen = now;
    const uint64_t step = std::max<uint64_t>(1, std::min<uint64_t>(10000, g_verify_timeout / 4));
    record.set("waited_ms", (int64_t)(wait.ms / step * step));
    return wait.ms;
}

void end_wait(lib::Json& record) {
    g_waits.erase(record["path"].str());
    record.erase("waited_ms");
}

// A stalled install is still looked at, for a registration that arrives late,
// but not at every poll: each look reads the installed packages.
bool too_soon(const std::string& path, bool force) {
    Wait& wait = g_waits[path];
    const uint64_t now = platform::now_ms();
    if (!force && wait.checked && now - wait.checked < kSlowCheckMs) return true;
    wait.checked = now ? now : 1;
    return false;
}

bool read_local_sfo(const std::string& path, std::string& bytes) {
    FILE* file = lib::open_file(path, "rb");
    if (!file) return false;
    const bool ok = read_package_entry([&](uint64_t at, uint64_t length, std::string& data) {
        if (fseeko(file, (off_t)at, SEEK_SET) != 0) return false;
        data.resize((size_t)length);
        return fread(&data[0], 1, (size_t)length, file) == length;
    }, 0x1000, bytes);
    fclose(file);
    return ok;
}

std::string installed_version(const std::string& title_id) {
    Item item;
    item.title_id = title_id;
    const std::string sfo = installed_app_sfo(item);
    return sfo_string(sfo, "TITLE_ID") == title_id ? sfo_string(sfo, "APP_VER") : std::string();
}
}

void set_install_verify_timeout(uint64_t ms) { g_verify_timeout = ms ? ms : kInstallVerifyTimeoutMs; }

#ifdef LIB_HOST
void host_reload_pending_installs() {
    g_pending_path.clear();
    g_polled_at = 0;
}
#endif

bool installation_uses_file(const std::string& path) {
    load_pending();
    for (const auto& pending : g_pending.items()) if (pending["path"].str() == path) return true;
    return false;
}

InstallTaskProgress installation_progress(const std::string& path) {
    load_pending();
    const auto found = g_install_progress.find(path);
    return found == g_install_progress.end() ? InstallTaskProgress{} : found->second;
}

bool delete_downloaded_package(const std::string& path, std::string& problem) {
    return downloads().delete_files(path, problem);
}

InstallResult install_local(const std::string& file, const Item& item) {
    if (const std::string self = self_install_problem(file, item); !self.empty()) return {false, self};
    if (installation_uses_file(file)) return {false, "This PKG is already being installed."};
    if (app_removal_pending(item)) return {false, "Wait for the console to finish removing this app before installing it again."};
    if (!lib::ends_with(lib::lower(file), ".pkg") || Downloads::file_state(item, file) != Downloads::FileState::Complete)
        return {false, "This download is incomplete or is not a PS4 package. Delete its download files and download it again."};
    // What is installed now: when the console cannot report progress, only a
    // version that was not there before proves the install happened.
    const std::string baseline = installed_version(item.title_id);
    auto result = platform_install_local(file, item);
    downloads().set_install_status(file, result.ok ?
        (result.task_id >= 0 ? DownloadInstallState::Installing : DownloadInstallState::None) : DownloadInstallState::Failed,
        result.message);
    if (result.task_id < 0) {
        if (!result.ok) update_install_progress(file, {InstallTaskState::Failed, result.message});
        return result;
    }
    std::string sfo;
    read_local_sfo(file, sfo);
    auto record = lib::Json::object();
    record.set("path", file); record.set("id", item.title_id); record.set("name", item.name);
    record.set("task", result.task_id); record.set("category", (int)item.category);
    record.set("start_failed", !result.ok);
    record.set("size", (int64_t)lib::file_size(file));
    record.set("version", sfo_string(sfo, "TITLE_ID") == item.title_id ? sfo_string(sfo, "APP_VER") : "");
    record.set("baseline", baseline);
    record.set("cleanup", catalog()->config().cleanup_after_install);
    load_pending(); g_pending.push(record); save_pending();
    g_waits.erase(file);
    update_install_progress(file, {InstallTaskState::Pending, "Preparing installation..."});
    return result;
}

std::vector<std::string> installer_tick(bool force) {
    std::vector<std::string> messages;
    load_pending();
    const uint64_t now = platform::now_ms();
    if (!force && now - g_polled_at < 1500) return messages;
    g_polled_at = now;
    lib::Json remaining = lib::Json::array();
    for (const auto& pending : g_pending.items()) {
        const std::string name = pending["name"].str(), path = pending["path"].str();
        const int32_t task = (int32_t)pending["task"].integer(-1);
        const bool previously_failed = pending["failed"].boolean() || pending["start_failed"].boolean();
        const bool stalled = pending["stalled"].boolean(), release_stalled = pending["release_stalled"].boolean();
        // Anything already brought to the user's attention is looked at less often.
        if ((stalled || release_stalled || pending["failure_reported"].boolean()) && too_soon(path, force)) {
            remaining.push(pending); continue;
        }
        const auto progress = previously_failed ? InstallTaskProgress{InstallTaskState::Failed, pending["failure"].str("The install did not start.")} :
            query_install_task(task);

        if (progress.state == InstallTaskState::Pending) {
            // Copying (or copying again): no wait for the console is running.
            auto record = pending;
            end_wait(record);
            if (stalled) {
                record.erase("stalled");
                downloads().set_install_status(path, DownloadInstallState::Installing, "Installing");
            }
            update_install_progress(path, progress);
            remaining.push(record); continue;
        }
        if (progress.state == InstallTaskState::Failed) {
            std::string problem;
            const bool released = release_install_task(task, true, problem);
            const std::string message = "Download complete; installation failed. " + progress.message +
                " Downloaded PKG kept." + (released ? " Failed task cleared; retry Install." : " " + problem);
            downloads().set_install_status(path, DownloadInstallState::Failed, message);
            if (!pending["failure_reported"].boolean() || released)
                messages.push_back(name + ": " + message);
            InstallTaskProgress shown{InstallTaskState::Failed, message};
            if (!released) {
                auto record = pending;
                end_wait(record);
                record.erase("stalled");
                record.set("failed", true); record.set("failure", progress.message); record.set("failure_reported", true);
                remaining.push(record);
                // The file stays protected, and the user has a way out (the
                // recovery actions) instead of a silent retry for ever.
                shown.attention = InstallAttention::FailedTaskHeld;
            } else {
                g_waits.erase(path);
            }
            update_install_progress(path, shown);
            if (!pending["failure_reported"].boolean()) LOG("installer: task %lld failed: %s; kept %s", (long long)pending["task"].integer(), progress.message.c_str(), path.c_str());
            continue;
        }

        // Complete, or the console cannot say (Unknown). Copying alone never
        // counts: the installed package must be the version that was sent,
        // including when an older version was already installed.
        const bool copied = progress.state == InstallTaskState::Complete;
        Item item; item.title_id = pending["id"].str();
        item.category = (Category)pending["category"].integer();
        refresh_installed(true);
        const std::string installed = installed_app_sfo(item), expected = pending["version"].str();
        const bool version_matches = !expected.empty() && sfo_string(installed, "TITLE_ID") == item.title_id &&
            compare_app_versions(sfo_string(installed, "APP_VER"), expected) == 0;
        // Without a progress report, a version that was already installed
        // before this install started proves nothing.
        const bool newly_installed = pending.has("baseline") &&
            compare_app_versions(pending["baseline"].str(), expected) != 0;
        const bool verified = version_matches && (copied || newly_installed);
        // A package whose own version cannot be read has nothing to compare:
        // once copied, its task is released and it is reported as unverified.
        const bool unverifiable = copied && expected.empty();
        if (!verified && !unverifiable) {
            auto record = pending;
            record.erase("verified");
            const uint64_t ms = waited(record);
            InstallTaskProgress shown = progress;
            if (copied) {
                shown.state = InstallTaskState::Pending;
                shown.percent = 99;
                shown.message = "Finalizing installation...";
            }
            if (stalled || ms >= g_verify_timeout) {
                const std::string message = stall_message(record, copied);
                if (!stalled) {
                    record.set("stalled", true);
                    downloads().set_install_status(path, DownloadInstallState::Unverified, message);
                    messages.push_back(message);
                    LOG("installer: task %d: version %s not registered after %llu ms (%s); kept %s", task, expected.c_str(),
                        (unsigned long long)ms, copied ? "copy complete" : "no progress report", path.c_str());
                }
                shown.message = message;
                shown.attention = InstallAttention::VerificationStalled;
            }
            update_install_progress(path, shown);
            remaining.push(record); continue;
        }

        std::string release_problem;
        if (!release_install_task(task, false, release_problem)) {
            auto record = pending;
            if (!record["release_reported"].boolean()) messages.push_back(name + ": install copy finished. " + release_problem);
            record.set("release_reported", true);
            if (!record["verified"].boolean()) {
                // A new wait: this one is for the task, not the version.
                end_wait(record);
                record.erase("stalled");
                record.set("verified", true);
            }
            const uint64_t ms = waited(record);
            InstallTaskProgress shown{InstallTaskState::Pending, "Finalizing installation...", 99, progress.done, progress.total};
            if (release_stalled || ms >= g_verify_timeout) {
                const std::string message = release_stall_message(record, release_problem);
                if (!release_stalled) {
                    record.set("release_stalled", true);
                    // Verified by version, so it is installed; only the PKG
                    // and its ownership record stay until the task is gone.
                    downloads().set_install_status(path, verified ? DownloadInstallState::Installed : DownloadInstallState::Unverified, message);
                    messages.push_back(message);
                    LOG("installer: task %d not released after %llu ms; kept %s", task, (unsigned long long)ms, path.c_str());
                }
                shown.message = message;
                shown.attention = InstallAttention::ReleaseStalled;
            }
            update_install_progress(path, shown);
            remaining.push(record);
            continue;
        }
        g_waits.erase(path);
        bool removed = false;
        std::string source_sfo;
        const bool same_package = read_local_sfo(path, source_sfo) && sfo_string(source_sfo, "TITLE_ID") == item.title_id &&
            compare_app_versions(sfo_string(source_sfo, "APP_VER"), expected) == 0;
        if (catalog()->config().cleanup_after_install && pending["cleanup"].boolean() && !expected.empty() &&
            same_package &&
            lib::file_size(path) == (uint64_t)pending["size"].integer() &&
            lib::ends_with(lib::lower(path), ".pkg")) removed = lib::remove_file(path);
        const std::string message = expected.empty() ? name + ": install copy completed; version could not be verified. PKG kept." :
            name + " installed." + (removed ? " Downloaded PKG deleted." : " Downloaded PKG kept.");
        downloads().set_install_status(path, expected.empty() ? DownloadInstallState::Unverified : DownloadInstallState::Installed, message);
        update_install_progress(path, expected.empty() ? InstallTaskProgress{InstallTaskState::Unknown, message} :
            InstallTaskProgress{InstallTaskState::Complete, message, 100, progress.done, progress.total});
        messages.push_back(message);
        LOG("installer: task %lld completed, %s PKG %s", (long long)pending["task"].integer(), removed ? "deleted" : "kept", path.c_str());
    }
    if (remaining.dump() != g_pending.dump()) { g_pending = remaining; save_pending(); platform::request_redraw(); }
    return messages;
}

bool recheck_installation(const std::string& path, std::string& message) {
    load_pending();
    bool found = false;
    for (auto& record : g_pending.items()) {
        if (record["path"].str() != path) continue;
        found = true;
        // The wait starts again; the ownership record itself is untouched.
        record.erase("stalled");
        record.erase("release_stalled");
        record.erase("waited_ms");
    }
    if (!found) {
        message = "No install is waiting on this PKG.";
        return false;
    }
    g_waits.erase(path);
    save_pending();
    const auto notices = installer_tick(true);
    if (!installation_uses_file(path)) {
        message = notices.empty() ? "The install task was released." : notices.back();
        return true;
    }
    const auto progress = installation_progress(path);
    if (progress.state == InstallTaskState::Failed) message = progress.message;
    else message = "Checking again. The PKG stays protected until the console confirms the install.";
    return true;
}

bool abandon_installation(const std::string& path, bool forget_unreleased, std::string& message) {
    load_pending();
    lib::Json remaining = lib::Json::array();
    bool found = false, done = false;
    for (const auto& record : g_pending.items()) {
        if (found || record["path"].str() != path) { remaining.push(record); continue; }
        found = true;
        const std::string name = record["name"].str();
        const bool failed = record["failed"].boolean() || record["start_failed"].boolean();
        if (!failed && !record["stalled"].boolean() && !record["release_stalled"].boolean()) {
            message = name + " is still installing. Wait for it to finish.";
            remaining.push(record); continue;
        }
        const int32_t task = (int32_t)record["task"].integer(-1);
        const bool verified = record["verified"].boolean() && !record["version"].str().empty();
        // A task that finished copying is only unregistered; any other is stopped first.
        std::string problem;
        const bool copied = !failed && query_install_task(task).state == InstallTaskState::Complete;
        const bool released = release_install_task(task, !copied, problem);
        if (!released && !forget_unreleased) {
            message = name + ": " + problem + " Remove its entry from Notifications > Downloads on the console and try again, "
                      "or choose Forget install task once it is gone.";
            remaining.push(record); continue;
        }
        done = true;
        g_waits.erase(path);
        const std::string task_text = released ? "Install task released." :
            "The console did not confirm releasing the task; it is no longer tracked.";
        if (verified) {
            message = name + " is installed. " + task_text + " Downloaded PKG kept.";
            downloads().set_install_status(path, DownloadInstallState::Installed, message);
            update_install_progress(path, {InstallTaskState::Complete, message, 100});
        } else if (failed) {
            message = name + ": installation failed. " + task_text + " Downloaded PKG kept; retry Install.";
            downloads().set_install_status(path, DownloadInstallState::Failed, message);
            update_install_progress(path, {InstallTaskState::Failed, message});
        } else {
            message = name + ": stopped waiting. The installation was not verified. " + task_text +
                      " Downloaded PKG kept; retry Install.";
            downloads().set_install_status(path, DownloadInstallState::Unverified, message);
            update_install_progress(path, {InstallTaskState::Unknown, message});
        }
        // The PKG is never deleted here, whatever the cleanup setting says.
        LOG("installer: task %d %s by the user (%s); kept %s", task, released ? "released" : "forgotten",
            verified ? "verified" : failed ? "failed" : "unverified", path.c_str());
    }
    if (!found) message = "No install is waiting on this PKG.";
    if (done) { g_pending = remaining; save_pending(); platform::request_redraw(); }
    return done;
}

std::string package_file_name(const Item& item) {
    // FPKGi's naming, so its downloads are found and continued: only '/' (and
    // NUL) are removed from the name.
    std::string name;
    for (char c : item.name)
        if (c != '/' && c != '\0') name += c;
    if (name.size() > 200) name.resize(200);
    return "[" + item.title_id + "] " + name + ".pkg";
}

std::string package_path(const Item& item) {
    std::string dir = catalog()->config().download_path;
    if (dir.empty()) return "";  // USB mode is waiting for a connected drive/choice.
    if (!lib::ends_with(dir, "/")) dir += "/";
    return dir + package_file_name(item);
}

bool parse_package_header(const std::string& b, PackageHeader& out) {
    // PKG header (big-endian): magic at 0, content ID (36 bytes) at 0x40,
    // content type (32 bits) at 0x74.
    if (b.size() < 0x78 || memcmp(b.data(), "\x7F" "CNT", 4) != 0) return false;
    out.content_id.assign(b.data() + 0x40, strnlen(b.data() + 0x40, 36));
    const unsigned char* t = (const unsigned char*)b.data() + 0x74;
    out.content_type = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3];
    switch (out.content_type) {
        case 0x1B: out.package_type = "PS4AC"; break;
        case 0x1C: out.package_type = "PS4AL"; break;
        case 0x1E: out.package_type = "PS4DP"; break;
        default: out.package_type = "PS4GD"; break;  // 0x1A, and anything unexpected
    }
    return out.content_id.size() >= 16;
}

std::string self_install_problem(const std::string& source, const Item& item) {
    constexpr const char* kSelf = "FPKP00001";
    bool self = item.title_id == kSelf;
    // Local files are checked by their header too: a PKG found in a folder
    // (such as a downloaded update) has no catalogue title ID.
    if (!self && !source.empty() && !lib::starts_with(source, "http://") && !lib::starts_with(source, "https://")) {
        PackageHeader header;
        self = read_package_header(source, header) && header.content_id.find(std::string("-") + kSelf + "_") != std::string::npos;
    }
    if (!self) return "";
    LOG("installer: refused to install FPKGi+'s own package %s", source.c_str());
    return "FPKGi+ cannot install its own update while it is running. Close FPKGi+, then install the PKG from "
           "Debug Settings > Game > Package Installer.";
}

bool read_package_header(const std::string& source, PackageHeader& out) {
    std::string bytes;
    if (lib::starts_with(source, "http://") || lib::starts_with(source, "https://")) {
        // Streamed to a scratch file and stopped after the first block, in
        // case the server ignores the range and starts sending the whole
        // package. (resume: the cancelled .part file is kept to be read.)
        lib::HttpRequest request;
        request.url = source;
        request.headers.emplace_back("Range", "bytes=0-127");
        request.timeout_seconds = 15;
        const std::string temp = lib::join_path(platform::data_root(), "header-probe");
        lib::remove_file(temp + ".part");
        const lib::HttpResponse r =
            lib::http_download(request, temp, [](uint64_t done, uint64_t) { return done < 0x80; }, true);
        out.final_url = r.final_url;
        if (!lib::read_file(temp + ".part", bytes, 1 << 20) || bytes.empty()) lib::read_file(temp, bytes, 1 << 20);
        lib::remove_file(temp + ".part");
        lib::remove_file(temp);
        if (bytes.size() < 0x80) return false;
    } else {
        FILE* f = lib::open_file(source, "rb");
        if (!f) return false;
        char buf[0x80];
        const size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        bytes.assign(buf, n);
    }
    return parse_package_header(bytes, out);
}

bool read_package_entry(const PackageReader& read, uint32_t id, std::string& bytes) {
    const auto be32 = [](const std::string& b, size_t at) {
        return (uint32_t)(unsigned char)b[at] << 24 | (uint32_t)(unsigned char)b[at + 1] << 16 |
               (uint32_t)(unsigned char)b[at + 2] << 8 | (uint32_t)(unsigned char)b[at + 3];
    };
    std::string head;
    if (!read(0, 0x20, head) || head.size() < 0x20 || memcmp(head.data(), "\x7F" "CNT", 4) != 0) return false;
    const uint32_t count = be32(head, 0x10), table = be32(head, 0x18);
    if (count == 0 || count > 512 || table < 0x20) return false;
    std::string entries;
    if (!read(table, (uint64_t)count * 32, entries) || entries.size() < (size_t)count * 32) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = (size_t)i * 32;
        if (be32(entries, at) != id) continue;
        const uint32_t flags = be32(entries, at + 8), offset = be32(entries, at + 16), size = be32(entries, at + 20);
        if ((flags & 0x80000000u) || size < 8 || size > 8u * 1024 * 1024) return false;
        return read(offset, size, bytes) && bytes.size() == size;
    }
    return false;
}

bool read_package_icon(const PackageReader& read, std::string& png) {
    return read_package_entry(read, 0x1200, png) && memcmp(png.data(), "\x89PNG", 4) == 0;
}

bool is_package_file(const std::string& path) {
    FILE* f = lib::open_file(path, "rb");
    if (!f) return false;
    unsigned char magic[4] = {};
    const bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "\x7F" "CNT", 4) == 0;
    fclose(f);
    return ok;
}

InstallResult place_in_install_folder(const std::string& file, const Item& item) {
    InstallResult r;
    if (const std::string self = self_install_problem(file, item); !self.empty()) {
        r.message = self;
        return r;
    }
    if (!is_package_file(file)) {
        r.message = "This file is not a PS4 package.";
        return r;
    }
    // Downloads are saved where the console's Package Installer looks (the
    // console's /data/pkg or a USB drive's top folder): nothing to move.
    r.ok = true;
    r.message = item.name + " is ready: install it from Debug settings > Package installer.";
    return r;
}

bool installer_owns_task(int32_t task_id) {
    if (task_id < 0) return false;
    for (const auto& record : g_pending.items())
        if ((int32_t)record["task"].integer(-1) == task_id) return true;
    return false;
}

}  // namespace store
