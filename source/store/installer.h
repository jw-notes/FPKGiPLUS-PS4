// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Installing packages and knowing what is installed.
//
// Installs go through the console's own background-transfer service (Bgft),
// the same service the PlayStation Store uses: the package appears under
// Notifications › Downloads and installs from there. A local .pkg (downloaded
// by FPKGi+) is handed over by path; a remote one by URL, and the console
// downloads it itself ("Let the console download it" in Settings).
// The service is only reachable after GoldHEN's jailbreak (platform).
#include <functional>
#include <string>
#include <vector>

#include "store/catalog.h"

namespace store {

// Loads the install service. Call once, on the main thread, after the app has
// left the sandbox. False (and a reason in installer_problem) when it cannot.
bool installer_init();
bool installer_ready();
std::string installer_problem();

struct InstallResult {
    bool ok = false;
    std::string message;  // for a toast: "Installing Jetpack Joyride" or why it failed
    int32_t task_id = -1;
};
enum class InstallTaskState { Pending, Complete, Failed, Unknown };
// An install the user has to look at; the PKG and the record of the task stay
// until it is resolved (recheck_installation / abandon_installation).
//   VerificationStalled  copying finished (or the console stopped reporting)
//                        but the expected version was not registered in time
//   ReleaseStalled       installed and verified, but the console will not
//                        release the install task
//   FailedTaskHeld       the install failed and its task cannot be released
enum class InstallAttention { None, VerificationStalled, ReleaseStalled, FailedTaskHeld };
struct InstallTaskProgress {
    InstallTaskState state = InstallTaskState::Unknown;
    std::string message;
    int percent = -1; // unknown/preparing until the service reports a total
    uint64_t done = 0, total = 0;
    InstallAttention attention = InstallAttention::None;
};
InstallResult platform_install_local(const std::string& file, const Item& item);
InstallTaskProgress query_install_task(int32_t task_id);
// Called only for task IDs recorded when FPKGi+ registered an install.
bool release_install_task(int32_t task_id, bool stop, std::string& problem);
#ifdef LIB_HOST
void host_set_install_task_result(int32_t task_id, InstallTaskState state, bool releasable, int percent = -1);
#endif
bool installation_uses_file(const std::string& path);
// Latest polled progress, shared by download jobs and manually installed files.
InstallTaskProgress installation_progress(const std::string& path);
// Polls the installs FPKGi+ started. An install counts as successful only
// when the console registers the version that was sent: a finished copy alone
// never does. That check is bounded: after kInstallVerifyTimeoutMs of waiting
// (time the app was running, carried across restarts) the install is flagged
// VerificationStalled with what to do, instead of "Finalizing" for ever. It is
// still looked at now and then, so a registration that arrives late completes
// it normally. The same bound applies to a console that will not release a
// finished task (ReleaseStalled).
std::vector<std::string> installer_tick(bool force = false);
constexpr uint64_t kInstallVerifyTimeoutMs = 5 * 60 * 1000;
// For tests: a shorter bound (0: the default again).
void set_install_verify_timeout(uint64_t ms);
// "Check again": clears a stall and looks at the install at once, with a
// fresh wait. False when no install is recorded for `path`.
bool recheck_installation(const std::string& path, std::string& message);
// "Stop waiting": releases the owned task of a stalled or failed install and
// drops its record, so the PKG can be installed again or deleted. The PKG is
// never deleted here. When the console refuses to release the task the record
// and the file's protection stay, unless `forget_unreleased`: the user says
// the task is gone (removed in Notifications > Downloads). Refused for an
// install that is still running normally.
bool abandon_installation(const std::string& path, bool forget_unreleased, std::string& message);
#ifdef LIB_HOST
// For tests: reads pending-installs.json again, as a restart would.
void host_reload_pending_installs();
#endif
bool delete_downloaded_package(const std::string& path, std::string& problem);
std::string installed_app_sfo(const Item& item);
std::string package_file_name(const Item& item);
int launch_check_flags(bool skip_launcher_checks);
// Installs a package that is already downloaded, at the local path `file`. When
// the in-process install service is available it is used; otherwise the package
// is placed in GoldHEN's install folder (/data/pkg) for
// GoldHEN to install, which is the path that works while sandboxed.
InstallResult install_local(const std::string& file, const Item& item);
// Asks the console to download and install straight from `url` (needs the
// install service; unavailable while sandboxed).
InstallResult install_from_url(const std::string& url, const Item& item);
// True when install_from_url can work (the in-process service loaded).
bool can_install_from_url();

// Moves (or copies) the finished package `file` into GoldHEN's install folder
// (/data/pkg). Shared by both backends; same-filesystem moves are
// instant. Returns a message naming where it went.
InstallResult place_in_install_folder(const std::string& file, const Item& item);

// What is installed, by title ID: /user/app/<id> (games and apps),
// /user/patch/<id> (updates), /user/addcont/<id> (add-ons).
bool is_installed(const Item& item);
bool app_removal_pending(const Item& item);
void app_removal_started(const Item& item);
// False means the platform has no authoritative registration query available.
bool query_app_registered(const std::string& title_id, bool& exists);
// Starts an installed title, as the home screen would (the console closes
// FPKGi+ to run a game). `message` says why when it could not.
InstallResult launch_title(const Item& item);
// Installed apps are removed through AppInstUtil, never by deleting folders.
// A nonempty problem also prevents system titles and FPKGi+ from being removed.
std::string app_delete_problem(const Item& item);
InstallResult delete_app(const Item& item);
// Changes whenever the installed apps, updates or add-ons change.
uint64_t installed_revision();
// Title IDs with an installed app (/user/app/<id>).
std::vector<std::string> installed_app_ids();

// ---- blocking Sony's downloads over fPKG installs ----
// An official update (or a full re-download) from PlayStation Network does
// not run on a fake package, and replaces or breaks the game. The blocker
// looks at the console's download queue for titles installed as fPKGs and
// stops a download only when its source is a playstation.net URL: FPKGi+'s
// own installs, Package Installer installs and retail-licensed games are
// left alone.
//
// From the first 0x80 bytes of /user/app/<id>/app.pkg: its content ID, and
// whether it is a fake (debug-signed) package. Retail packages have the top
// bit of the header's type (offset 4, big-endian) set: 0x83000001 for a
// retail game against 0x40000001 for an fPKG. False when it cannot be read.
bool installed_app_package(const std::string& title_id, std::string& content_id, bool& fake);
bool package_header_is_fake(const std::string& header);
// True for a task FPKGi+ registered itself (a recorded install).
bool installer_owns_task(int32_t task_id);
// True when a value read from a download task names Sony's servers.
bool is_sony_source(const std::string& value);
// Called every frame by the app and every tick by the background service;
// checks the queue every few seconds. Messages are for toasts (the app also
// shows a console notification; the service shows its own).
// The service watches the queue whenever it can, so it works with FPKGi+
// closed and while games run; the app then only keeps the list of installed
// fPKGs up to date for it (<data>/service/fpkg-titles.json) and watches the
// queue itself when the service does not (service_blocker_active).
// FPKGIPLUS.blockSonyUpdates (Settings > Pre-flight checks) turns both off.
std::vector<std::string> update_blocker_tick(bool from_service = false);
// True while the background service reports that it is watching the queue.
bool service_blocker_active();
// Re-reads the folders above (after an install, or when the app is resumed).
// The folders are read again at most every few seconds, unless `force`.
void refresh_installed(bool force = false);

// Where the console keeps installed titles: "/user" on the console; on the
// host a folder under the data directory, for tests.
std::string system_root();

// The file FPKGi uses for a download: "<download path>[CUSA00001] Name.pkg".
std::string package_path(const Item& item);
// True when `path` starts like a PS4 package ("\x7F" "CNT").
bool is_package_file(const std::string& path);

// From the first 0x80 bytes of a package: its content ID
// ("UP0001-CUSA00001_00-...") and the install service's package type
// ("PS4GD" game, "PS4AC" add-on, "PS4AL" add-on licence, "PS4DP" update).
struct PackageHeader {
    std::string content_id, package_type;
    std::string final_url;  // a URL after its redirects (the install service is given this one)
    uint32_t content_type = 0;
};
bool parse_package_header(const std::string& bytes, PackageHeader& out);
// FPKGi+ never installs itself while it is running: its updates are installed
// by the user from GoldHEN's Package Installer. A message when `source` (a
// local file or URL) or `item` is an FPKGi+ package, otherwise "".
std::string self_install_problem(const std::string& source, const Item& item);
// Reads that header from a local file or, with an HTTP range request, a URL.
bool read_package_header(const std::string& source, PackageHeader& out);

// A package's own icon (icon0.png, entry 0x1200 of its entry table), read
// through `read` (offset, length, bytes) so that a remote package costs a few
// small range requests. False when there is none or it is encrypted.
using PackageReader = std::function<bool(uint64_t offset, uint64_t length, std::string& out)>;
bool read_package_icon(const PackageReader& read, std::string& png);
bool read_package_entry(const PackageReader& read, uint32_t id, std::string& bytes);

}  // namespace store
