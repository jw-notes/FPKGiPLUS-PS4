// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The download queue: packages downloaded to FPKGi's download folder one at a
// time, on a thread of their own, then (optionally) installed. A download that
// stops (network, the app closed) is continued from where it stopped; so is a
// half-finished FPKGi download ("<file>.pkg.resume").
//
// Queued, paused and running downloads are kept in <data>/download-queue.json
// (order, the title's details and the file each one is saved to) and come back
// when the app is opened again: see Downloads::restore.
#include <stdint.h>

#include <string>
#include <vector>

#include "store/catalog.h"

namespace store {

enum class JobState { Queued, Downloading, Paused, Done, Failed, Cancelled };
// Unverified: the console finished copying but the installed version could
// not be confirmed (see installer.h); the package is kept.
enum class DownloadInstallState { None, Installing, Installed, Failed, Unverified };

struct Job {
    uint64_t id = 0;
    Item item;
    std::string path;  // the finished file
    JobState state = JobState::Queued;
    uint64_t done = 0, total = 0;
    double speed = 0;  // bytes per second, smoothed
    std::string message;  // why it failed or waits, or what happened after it finished
    bool installed = false;
    DownloadInstallState install_state = DownloadInstallState::None;
    std::string install_message;
};

// ---- where a download is saved ---------------------------------------------------

// The USB slot ("/mnt/usb1/") `path` is on; "" for the console's own storage.
std::string usb_drive_of(const std::string& path);
// "Console", "USB drive 1".
std::string destination_name(const std::string& path);
// False when `path` is on a USB drive that is not connected now. A download
// is never moved to another place because of it: it waits for its drive.
bool destination_available(const std::string& path);

// ---- free space ------------------------------------------------------------------

// Unknown: the storage does not say how much is free (a drive reached through
// FTP, a console that does not answer). That is not "full": only Insufficient
// stops a download.
enum class StorageVerdict { Enough, Insufficient, Unknown };
struct StorageCheck {
    StorageVerdict verdict = StorageVerdict::Unknown;
    uint64_t needed = 0;     // bytes still to be written
    uint64_t available = 0;  // free bytes (when known)
    uint64_t shortfall = 0;  // bytes to free before it fits (Insufficient)
};
// Kept free beyond the download itself, for the file system and resume notes.
constexpr uint64_t kStorageReserve = 16ull * 1024 * 1024;
// `remaining`: what is still to be written, i.e. the size less the bytes
// already saved (lib::http_partial_bytes, which counts every connection's).
StorageCheck check_storage(uint64_t remaining, bool free_known, uint64_t free_bytes);
// The same for the storage that holds the file `path`.
StorageCheck destination_storage(const std::string& path, uint64_t remaining);
// "Not enough free space on Console: ..." for an Insufficient check.
std::string storage_problem(const StorageCheck& check, const std::string& path);

class Downloads {
public:
    void start();
    // Stops the worker. A download running at that moment keeps its partial
    // file and becomes Paused ("interrupted"), ready to resume.
    void stop();
    // Loads the saved queue. Call once after the data folder and USB access
    // are set up and before start(); nothing is saved before it has run, so a
    // queue left by the last session cannot be overwritten unread.
    //
    // Every entry is validated (address, title, a destination that is the
    // console's download folder or a USB slot, FPKGi's own file name) and
    // dropped if it is not sound. Queued and paused downloads return as they
    // were, in order. One that was running when the app closed or crashed
    // returns Paused: its partial file and resume notes are untouched and
    // Resume continues it. One whose USB drive is not connected returns Paused
    // with that reason and keeps its destination.
    void restore();
    // Adds a download (ignored if the same package is already queued or running).
    bool add(const Item& item);
    // Pauses a queued or running download; the partial file is kept and it
    // stays in the list, ready to resume.
    void pause(const std::string& url);
    // Resumes or retries a job at its original destination. False (and why in
    // `problem`) when that destination is a USB drive that is not connected.
    bool resume(const std::string& url, std::string* problem = nullptr);
    // Cancels a queued or running download; the partial file is kept so it can
    // be continued later, but it leaves the active list.
    void cancel(const std::string& url);
    // Removes finished, failed and cancelled entries from the list.
    void clear_finished(const std::vector<std::string>& files = {});
    // Delete a stopped download and all partial/resume data, then forget its jobs.
    // Active transfers and packages still being installed are protected.
    bool delete_files(const std::string& path, std::string& problem);
    uint64_t revision() const;
    void set_install_status(const std::string& path, DownloadInstallState state, const std::string& message);
    // Cleared file rows stay hidden after rescanning/restarting. Files remain
    // on disk and a new download makes its path visible again.
    bool file_cleared(const std::string& path, uint64_t size) const;
    bool contains(const std::string& url) const;
    // A copy of the list, for drawing.
    std::vector<Job> snapshot() const;
    int active() const;  // queued + downloading
    // State of the package as the download folder shows it.
    enum class FileState { None, Partial, Complete };
    static FileState file_state(const Item& item, const std::string& path = "");
    // Called on the UI thread once per frame: installs finished downloads
    // (when "Install when downloaded" is on) and reports them.
    std::vector<std::string> tick();  // messages to show
#ifdef LIB_HOST
    // For tests: forgets the list as closing the app would, without saving.
    // Only while the worker is stopped.
    void host_forget();
#endif
};

Downloads& downloads();

}  // namespace store
