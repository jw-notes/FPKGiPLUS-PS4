// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "store/downloads.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <set>
#include <mutex>
#include <thread>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "core/usb_ftp.h"
#include "net/http.h"
#include "platform/platform.h"
#include "store/installer.h"

namespace store {

namespace {

std::mutex g_lock;
std::condition_variable g_wake;
std::vector<Job> g_jobs;
std::thread g_thread;
bool g_running = false;
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_cancel_current{false}, g_pause_current{false};
std::string g_current_url;
// Finished downloads waiting for the UI thread (install, toast).
std::deque<uint64_t> g_finished;
uint64_t g_next_id = 1;
std::atomic<uint64_t> g_revision{1};
lib::Json g_cleared = lib::Json::object();
std::string g_cleared_file;
// The saved queue: its file (empty until restore() has read it) and the text
// last written, so that only a change is written.
std::string g_queue_file, g_queue_saved;

const char kInterrupted[] = "Interrupted when FPKGi+ closed. Resume to continue from where it stopped.";
constexpr size_t kMaxSavedJobs = 512;
constexpr size_t kMaxSavedText = 8192;

void load_cleared() {
    const std::string path = lib::join_path(platform::data_root(), "cleared-downloads.json");
    if (path == g_cleared_file) return;
    g_cleared_file = path;
    std::string text;
    bool ok = false;
    g_cleared = lib::read_file(path, text, 1 << 20) ? lib::Json::parse(text, &ok) : lib::Json();
    if (!ok || !g_cleared.is_object()) g_cleared = lib::Json::object();
}

void show_file(const std::string& path) {
    load_cleared();
    bool changed = false;
    for (const auto& file : {path, path + ".part", path + ".resume"}) {
        if (g_cleared.has(file)) { g_cleared.erase(file); changed = true; }
    }
    if (changed) lib::write_file_atomic(g_cleared_file, g_cleared.dump());
}

Job* by_id(uint64_t id) {
    for (auto& j : g_jobs)
        if (j.id == id) return &j;
    return nullptr;
}

bool active_state(JobState s) { return s == JobState::Queued || s == JobState::Downloading; }
// In play: queued, running or paused. These are the jobs that are saved.
bool live_state(JobState s) { return active_state(s) || s == JobState::Paused; }

Job* find(const std::string& url) {
    for (auto& j : g_jobs)
        if (j.item.url == url && active_state(j.state)) return &j;
    return nullptr;
}

// Any job still in play (queued, running or paused).
Job* find_live(const std::string& url) {
    for (auto& j : g_jobs)
        if (j.item.url == url && live_state(j.state)) return &j;
    return nullptr;
}

std::string missing_destination(const std::string& path) {
    return destination_name(path) + " is not connected. Reconnect it, then resume: this download stays on that drive.";
}

// ---- the saved queue ---------------------------------------------------------

lib::Json item_json(const Item& item) {
    lib::Json j = lib::Json::object();
    j.set("url", item.url);
    j.set("title_id", item.title_id);
    j.set("name", item.name);
    j.set("region", item.region);
    j.set("version", item.version);
    j.set("release", item.release);
    j.set("min_fw", item.min_fw);
    j.set("cover_url", item.cover_url);
    j.set("release_date", item.release_date);
    j.set("release_date_scope", item.release_date_scope);
    j.set("latest_release_date", item.latest_release_date);
    j.set("description", item.description);
    j.set("developer", item.developer);
    j.set("publisher", item.publisher);
    j.set("genres", item.genres);
    j.set("players", item.players);
    j.set("languages", item.languages);
    j.set("rating", item.rating);
    j.set("metadata_source", item.metadata_source);
    j.set("homebrew_category", item.homebrew_category);
    j.set("github_repo", item.github_repo);
    j.set("github_asset", item.github_asset);
    j.set("size", (int64_t)item.size);
    j.set("category", (int)item.category);
    j.set("ps5", item.ps5);
    return j;
}

bool plain_text(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 && c != '\n' && c != '\t') return false;
    return true;
}

// A saved title, or why it cannot be used.
const char* item_from_json(const lib::Json& j, Item& item) {
    if (!j.is_object()) return "no title details";
    const auto text = [&](const char* key, std::string& out) {
        out = j[key].str();
        if (out.size() > kMaxSavedText) out.resize(lib::utf8_prefix(out, kMaxSavedText / 4));
        return plain_text(out);
    };
    bool ok = text("url", item.url);
    ok = text("title_id", item.title_id) && ok;
    ok = text("name", item.name) && ok;
    ok = text("region", item.region) && ok;
    ok = text("version", item.version) && ok;
    ok = text("release", item.release) && ok;
    ok = text("min_fw", item.min_fw) && ok;
    ok = text("cover_url", item.cover_url) && ok;
    ok = text("release_date", item.release_date) && ok;
    ok = text("release_date_scope", item.release_date_scope) && ok;
    ok = text("latest_release_date", item.latest_release_date) && ok;
    ok = text("description", item.description) && ok;
    ok = text("developer", item.developer) && ok;
    ok = text("publisher", item.publisher) && ok;
    ok = text("genres", item.genres) && ok;
    ok = text("players", item.players) && ok;
    ok = text("languages", item.languages) && ok;
    ok = text("rating", item.rating) && ok;
    ok = text("metadata_source", item.metadata_source) && ok;
    ok = text("homebrew_category", item.homebrew_category) && ok;
    ok = text("github_repo", item.github_repo) && ok;
    ok = text("github_asset", item.github_asset) && ok;
    if (!ok) return "control characters in the title's details";
    if (item.homebrew_category.empty()) item.homebrew_category = "Other";
    if (!(lib::starts_with(item.url, "http://") || lib::starts_with(item.url, "https://")) || item.url.size() < 10 ||
        item.url.size() > 4096 || item.url.find_first_of(" \n\t") != std::string::npos)
        return "not an http(s) address";
    if (item.title_id.empty() || item.title_id.size() > 32 || item.title_id.find_first_of("/\\\n\t") != std::string::npos)
        return "no usable title ID";
    if (item.name.empty() || item.name.find('\n') != std::string::npos) return "no title name";
    const int64_t size = j["size"].integer(-1), category = j["category"].integer(-1);
    if (size < 0) return "no valid size";
    if (category < 0 || category >= (int64_t)Category::Count) return "unknown category";
    item.size = (uint64_t)size;
    item.category = (Category)category;
    item.ps5 = j["ps5"].boolean();
    return nullptr;
}

// A destination a download may have: the console's download folder or the
// top folder of a USB slot, and the file name FPKGi+ gives that title.
bool valid_destination(const std::string& path, const Item& item) {
    const std::string name = package_file_name(item);
    if (path.size() <= name.size() || !lib::ends_with(path, name)) return false;
    const std::string folder = path.substr(0, path.size() - name.size());
    if (folder == console_download_path()) return true;
    for (const auto& slot : platform::usb_mount_points())
        if (folder == slot + "/") return true;
    return false;
}

// Under g_lock. Writes the jobs still in play, in order, when they changed.
void save_queue() {
    if (g_queue_file.empty()) return;  // restore() has not read the old one yet
    lib::Json list = lib::Json::array();
    for (const auto& job : g_jobs) {
        if (!live_state(job.state)) continue;
        lib::Json entry = lib::Json::object();
        entry.set("state", job.state == JobState::Queued ? "queued" : job.state == JobState::Paused ? "paused" : "active");
        entry.set("path", job.path);
        entry.set("total", (int64_t)job.total);
        entry.set("item", item_json(job.item));
        list.push(entry);
    }
    lib::Json root = lib::Json::object();
    root.set("version", 1);
    root.set("jobs", list);
    const std::string text = root.dump();
    if (text == g_queue_saved) return;
    if (lib::write_file_atomic(g_queue_file, text)) g_queue_saved = text;
    else LOG("downloads: could not save the queue to %s", g_queue_file.c_str());
}

// Under g_lock, after the list or a job's state changed.
void changed() {
    ++g_revision;
    save_queue();
}

void run_one(uint64_t id) {
    Item item;
    std::string path;
    {
        std::lock_guard<std::mutex> l(g_lock);
        Job* job = by_id(id);
        if (!job || job->state != JobState::Queued) return;
        item = job->item;
        path = job->path;
        // Its drive was unplugged: it waits there. Nothing is created at the
        // empty mount point (which is the console's own storage) and it is
        // not moved to wherever downloads are saved now.
        if (!destination_available(path)) {
            job->state = JobState::Paused;
            job->message = missing_destination(path);
            LOG("downloads: %s waits: %s", item.title_id.c_str(), job->message.c_str());
            changed();
            platform::request_redraw();
            return;
        }
        job->state = JobState::Downloading;
        job->message.clear();
        g_cancel_current = false;
        g_pause_current = false;
        g_current_url = item.url;
        changed();
    }
    lib::make_dirs(lib::parent_dir(path));
    // FPKGi's own partial file is continued (it holds the start of the package).
    const std::string part = path + ".part", fpkgi_part = path + ".resume";
    if (!lib::file_exists(part) && lib::file_exists(fpkgi_part)) {
        if (lib::rename_file(fpkgi_part, part)) LOG("downloads: continuing FPKGi's partial file for %s", item.title_id.c_str());
    }
    LOG("downloads: %s (%s)", item.title_id.c_str(), lib::format_bytes(item.size).c_str());

    lib::HttpRequest request;
    request.url = item.url;
    request.timeout_seconds = 60;
    uint64_t last_ms = platform::now_ms(), last_bytes = 0;
    double speed = 0;
    bool first = true;
    // Several connections at once, except onto a USB drive: one reached
    // through FTP cannot be written out of order, and exFAT has to fill every
    // gap it is asked to skip.
    const bool in_order = lib::usb_ftp::handles(path) || lib::starts_with(path, "/mnt/usb");
    const lib::HttpResponse r = lib::http_download_parallel(
        request, path,
        [&](uint64_t done, uint64_t total) {
            const uint64_t now = platform::now_ms();
            if (first) {
                first = false;
                last_bytes = done;  // a continued download starts part-way
                last_ms = now;
            }
            if (now - last_ms >= 500) {
                const double instant = (double)(done - last_bytes) * 1000.0 / (double)(now - last_ms);
                speed = speed == 0 ? instant : speed * 0.75 + instant * 0.25;
                last_ms = now;
                last_bytes = done;
                std::lock_guard<std::mutex> l(g_lock);
                Job* jp = by_id(id);
                if (!jp) return !g_cancel_current && !g_pause_current && !g_stopping;
                Job& j = *jp;
                j.done = done;
                j.total = total ? total : item.size;
                j.speed = speed;
                platform::request_redraw();
            }
            return !g_cancel_current && !g_pause_current && !g_stopping;
        },
        catalog()->config().connections, in_order,
        // Asked once the server has given the real size, before a byte is
        // written: `remaining` already leaves out what is saved.
        [&](uint64_t remaining, uint64_t total, std::string& error) {
            const StorageCheck check = destination_storage(path, remaining);
            if (check.verdict == StorageVerdict::Unknown) {
                LOG("downloads: free space on %s is not reported; continuing with %s (%s to save)", destination_name(path).c_str(),
                    item.title_id.c_str(), total ? lib::format_bytes(remaining).c_str() : "an unknown size");
                return true;
            }
            if (check.verdict == StorageVerdict::Enough) return true;
            error = storage_problem(check, path);
            return false;
        });

    std::lock_guard<std::mutex> l(g_lock);
    g_current_url.clear();
    Job* jp = by_id(id);
    if (!jp) return;
    Job& j = *jp;
    j.speed = 0;
    if (g_cancel_current) {
        j.state = JobState::Cancelled;
        j.message = "Stopped. Downloading it again continues from here.";
    } else if (g_stopping) {
        // The app is closing: the same state a restart gives a download that
        // was cut off, so what is shown and what is saved agree.
        j.state = JobState::Paused;
        j.message = kInterrupted;
    } else if (g_pause_current) {
        j.state = JobState::Paused;
        j.message = "Paused.";
    } else if (!r.ok()) {
        j.state = JobState::Failed;
        j.message = r.insufficient_space ? r.error : lib::describe_failure(r);
        LOG("downloads: %s failed: %s", item.title_id.c_str(), j.message.c_str());
    } else if (!is_package_file(path)) {
        j.state = JobState::Failed;
        j.message = "The server did not send a PS4 package (the file was removed).";
        lib::remove_file(path);
    } else {
        j.state = JobState::Done;
        j.done = j.total = lib::file_size(path);
        j.message = "Downloaded to " + path;
        if (r.remote_changed) j.message += " (the file had changed on the server, so it was downloaded again from the start)";
        g_finished.push_back(id);
        LOG("downloads: %s done", item.title_id.c_str());
    }
    if (live_state(j.state) || j.state == JobState::Failed || j.state == JobState::Cancelled) {
        // What is really saved, for the row and for the next attempt.
        const uint64_t saved = lib::http_partial_bytes(path);
        if (saved || j.state != JobState::Paused) j.done = j.total ? std::min(saved, j.total) : saved;
    }
    changed();
    platform::request_redraw();
}

void worker() {
    platform::background_thread_priority();
    for (;;) {
        uint64_t next = 0;
        {
            std::unique_lock<std::mutex> l(g_lock);
            g_wake.wait(l, [] {
                if (g_stopping) return true;
                for (const auto& j : g_jobs)
                    if (j.state == JobState::Queued) return true;
                return false;
            });
            if (g_stopping) return;
            for (const auto& j : g_jobs)
                if (j.state == JobState::Queued) {
                    next = j.id;
                    break;
                }
        }
        if (next) run_one(next);
    }
}

}  // namespace

// ---- destinations and free space ---------------------------------------------

std::string usb_drive_of(const std::string& path) {
    for (const auto& slot : platform::usb_mount_points())
        if (lib::starts_with(path, slot + "/")) return slot + "/";
    return "";
}

std::string destination_name(const std::string& path) {
    const auto slots = platform::usb_mount_points();
    for (size_t i = 0; i < slots.size(); ++i)
        if (lib::starts_with(path, slots[i] + "/")) return "USB drive " + std::to_string(i);
    if (lib::starts_with(path, "/mnt/usb") && path.size() > 8 && path[8] >= '0' && path[8] <= '7')
        return std::string("USB drive ") + path[8];
    return "Console";
}

bool destination_available(const std::string& path) {
    const std::string drive = usb_drive_of(path);
    if (drive.empty()) return true;
    const auto drives = connected_usb_drives();
    return std::find(drives.begin(), drives.end(), drive) != drives.end();
}

StorageCheck check_storage(uint64_t remaining, bool free_known, uint64_t free_bytes) {
    StorageCheck check;
    check.needed = remaining;
    check.available = free_known ? free_bytes : 0;
    if (remaining == 0) {
        check.verdict = StorageVerdict::Enough;  // nothing left to write
    } else if (!free_known) {
        check.verdict = StorageVerdict::Unknown;
    } else if (free_bytes >= kStorageReserve && free_bytes - kStorageReserve >= remaining) {
        check.verdict = StorageVerdict::Enough;
    } else {
        check.verdict = StorageVerdict::Insufficient;
        check.shortfall = remaining + kStorageReserve - free_bytes;
    }
    return check;
}

StorageCheck destination_storage(const std::string& path, uint64_t remaining) {
    uint64_t free_bytes = 0;
    const bool known = lib::free_space(lib::parent_dir(path), free_bytes);
    return check_storage(remaining, known, free_bytes);
}

std::string storage_problem(const StorageCheck& check, const std::string& path) {
    return "Not enough free space on " + destination_name(path) + ": " + lib::format_bytes(check.needed) +
           " still to download, " + lib::format_bytes(check.available) + " free. Free at least " +
           lib::format_bytes(check.shortfall) + ", then retry. The partial download is kept.";
}

Downloads& downloads() {
    static Downloads d;
    return d;
}

void Downloads::start() {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_running) return;
    g_running = true;
    g_stopping = false;
    g_thread = std::thread(worker);
}

void Downloads::stop() {
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_running) return;
        g_stopping = true;
    }
    g_wake.notify_all();
    if (g_thread.joinable()) g_thread.join();
    g_running = false;
}

void Downloads::restore() {
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_queue_file = lib::join_path(platform::data_root(), "download-queue.json");
        g_queue_saved.clear();
        std::string text;
        bool ok = false;
        const lib::Json root = lib::read_file(g_queue_file, text, 8 << 20) ? lib::Json::parse(text, &ok) : lib::Json();
        if (!text.empty() && (!ok || !root.is_object() || root["version"].integer() != 1 || !root["jobs"].is_array())) {
            // Kept beside the new queue for a look; never half-used.
            LOG("downloads: the saved queue is not readable; set aside as %s.bad", g_queue_file.c_str());
            lib::remove_file(g_queue_file + ".bad");
            lib::rename_file(g_queue_file, g_queue_file + ".bad");
        } else if (ok) {
            size_t restored = 0, dropped = 0;
            std::set<std::string> paths;
            for (const auto& job : g_jobs) paths.insert(job.path);
            for (const auto& entry : root["jobs"].items()) {
                if (restored >= kMaxSavedJobs) { ++dropped; continue; }
                Job job;
                const std::string state = entry["state"].str();
                const char* why = !entry.is_object() ? "not an entry" : item_from_json(entry["item"], job.item);
                job.path = entry["path"].str();
                if (!why && state != "queued" && state != "paused" && state != "active") why = "unknown state";
                if (!why && !valid_destination(job.path, job.item)) why = "its destination is not a download folder";
                if (!why && (find_live(job.item.url) || paths.count(job.path))) why = "already in the list";
                if (!why && entry["total"].integer() < 0) why = "no valid size";
                if (why) {
                    ++dropped;
                    LOG("downloads: saved entry %s dropped: %s", lib::file_name(job.path).c_str(), why);
                    continue;
                }
                job.id = g_next_id++;
                job.total = (uint64_t)entry["total"].integer();
                if (!job.total) job.total = job.item.size;
                // Without its drive nothing about the files can be known, and
                // the download waits for it where it is.
                if (!destination_available(job.path)) {
                    job.state = JobState::Paused;
                    job.message = missing_destination(job.path);
                } else {
                    // What is on the disk decides, not what was last noted.
                    job.done = lib::http_partial_bytes(job.path);
                    if (job.total && job.done > job.total) job.done = job.total;
                    if (!job.done && file_state(job.item, job.path) == FileState::Complete) {
                        // It finished as the app closed: nothing to fetch again.
                        job.state = JobState::Done;
                        job.done = job.total = lib::file_size(job.path);
                        job.message = "Downloaded to " + job.path;
                        g_finished.push_back(job.id);
                    } else if (state == "active") {
                        // Cut off mid-transfer. The partial file and its
                        // notes are as they were left; it waits to be resumed.
                        job.state = JobState::Paused;
                        job.message = kInterrupted;
                    } else if (state == "paused") {
                        job.state = JobState::Paused;
                        job.message = "Paused.";
                    } else {
                        job.state = JobState::Queued;
                    }
                }
                paths.insert(job.path);
                show_file(job.path);
                g_jobs.push_back(std::move(job));
                ++restored;
            }
            LOG("downloads: restored %zu saved download(s)%s", restored,
                dropped ? (", dropped " + std::to_string(dropped) + " unusable").c_str() : "");
        }
        changed();
    }
    g_wake.notify_all();
    platform::request_redraw();
}

#ifdef LIB_HOST
void Downloads::host_forget() {
    std::lock_guard<std::mutex> l(g_lock);
    g_jobs.clear();
    g_finished.clear();
    g_queue_file.clear();
    g_queue_saved.clear();
    ++g_revision;
}
#endif

bool Downloads::add(const Item& item) {
    if (app_removal_pending(item)) return false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (Job* existing = find_live(item.url)) {
            if (existing->state != JobState::Paused) return false;
            // Resume instead, where it was being saved.
            if (!destination_available(existing->path)) {
                existing->message = missing_destination(existing->path);
                changed();
                return false;
            }
            existing->state = JobState::Queued;
            existing->message.clear();
        } else {
            auto& config = catalog()->config();
            if (!resolve_download_destination(config, config.download_to_usb ? connected_usb_drives() : std::vector<std::string>{}))
                return false;
            Job j;
            j.id = g_next_id++;
            j.item = item;
            j.path = package_path(item);
            if (installation_uses_file(j.path)) return false;
            show_file(j.path);
            j.total = item.size;
            j.done = lib::http_partial_bytes(j.path);
            if (j.total && j.done > j.total) j.done = j.total;
            g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [&](const Job& old) {
                return old.item.url == item.url && !live_state(old.state);
            }), g_jobs.end());
            g_finished.erase(std::remove_if(g_finished.begin(), g_finished.end(), [](uint64_t id) { return !by_id(id); }), g_finished.end());
            g_jobs.push_back(std::move(j));
        }
        changed();
    }
    g_wake.notify_all();
    return true;
}

void Downloads::pause(const std::string& url) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_current_url == url) {
        g_pause_current = true;
        return;
    }
    if (Job* j = find(url)) {  // queued but not started yet
        j->state = JobState::Paused;
        j->message = "Paused.";
        changed();
    }
}

bool Downloads::resume(const std::string& url, std::string* problem) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        Job* j = find_live(url);
        if (!j) {
            for (auto it = g_jobs.rbegin(); it != g_jobs.rend(); ++it) {
                if (it->item.url == url && (it->state == JobState::Failed || it->state == JobState::Cancelled)) {
                    j = &*it;
                    break;
                }
            }
        }
        if (!j) {
            if (problem) *problem = "This download is no longer in the list.";
            return false;
        }
        if (j->state == JobState::Paused || j->state == JobState::Failed || j->state == JobState::Cancelled) {
            if (!destination_available(j->path)) {
                const std::string why = missing_destination(j->path);
                if (j->state == JobState::Paused) j->message = why;
                if (problem) *problem = why;
                changed();
                return false;
            }
            j->state = JobState::Queued;
            j->message.clear();
            changed();
        }
    }
    g_wake.notify_all();
    return true;
}

void Downloads::cancel(const std::string& url) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_current_url == url) {
        g_cancel_current = true;
        return;
    }
    if (Job* j = find_live(url)) {
        j->state = JobState::Cancelled;
        j->message = "Removed from the queue.";
        changed();
    }
}

void Downloads::clear_finished(const std::vector<std::string>& files) {
    std::lock_guard<std::mutex> l(g_lock);
    // A clear requested by the user also consumes pending completion notices.
    g_finished.clear();
    load_cleared();
    auto hide = [](const std::string& path) {
        if (!path.empty() && lib::file_exists(path)) g_cleared.set(path, (int64_t)lib::file_size(path));
    };
    for (const auto& file : files) hide(file);
    for (const auto& j : g_jobs) {
        if (live_state(j.state)) continue;
        hide(j.path);
        hide(j.path + ".part");
        hide(j.path + ".resume");
    }
    lib::write_file_atomic(g_cleared_file, g_cleared.dump());
    g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [](const Job& j) { return !live_state(j.state); }),
                 g_jobs.end());
    changed();
}

uint64_t Downloads::revision() const { return g_revision.load(); }

void Downloads::set_install_status(const std::string& path, DownloadInstallState state, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& job : g_jobs) if (job.path == path && job.state == JobState::Done) {
        job.install_state = state;
        job.install_message = message;
        job.installed = state == DownloadInstallState::Installed;
    }
    platform::request_redraw();
}

bool Downloads::delete_files(const std::string& selected, std::string& problem) {
    std::string path = selected;
    for (const char* suffix : {".part.chunks", ".part.meta", ".resume.chunks", ".part", ".resume"}) {
        if (lib::ends_with(path, suffix)) { path.resize(path.size() - strlen(suffix)); break; }
    }
    if (!lib::ends_with(lib::lower(path), ".pkg")) { problem = "This is not a downloaded PKG file."; return false; }
    std::lock_guard<std::mutex> lock(g_lock);
    for (const auto& job : g_jobs) if (job.path == path &&
        (active_state(job.state) || g_current_url == job.item.url)) {
        problem = "Stop this download and wait for it to finish stopping before deleting its files.";
        return false;
    }
    if (installation_uses_file(path)) { problem = "The installer is still using this PKG. Wait for installation to finish."; return false; }
    // Delete the resume notes (the sparse-file map and the remote file's
    // validators) along with the partial bytes. Keeping either behind can
    // cause a later attempt to resume stale data.
    std::vector<std::string> files = lib::http_partial_files(path);
    files.insert(files.begin(), path);
    for (const auto& file : files) {
        if (lib::file_exists(file) && !lib::remove_file(file)) {
            problem = "Could not delete " + file + ".";
            ++g_revision;
            return false;
        }
    }
    std::set<uint64_t> removed;
    for (const auto& job : g_jobs) if (job.path == path) removed.insert(job.id);
    g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [&](const Job& job) { return job.path == path; }), g_jobs.end());
    g_finished.erase(std::remove_if(g_finished.begin(), g_finished.end(), [&](uint64_t id) { return removed.count(id) != 0; }), g_finished.end());
    show_file(path);
    changed();
    platform::request_redraw();
    problem.clear();
    return true;
}

bool Downloads::file_cleared(const std::string& path, uint64_t size) const {
    std::lock_guard<std::mutex> l(g_lock);
    load_cleared();
    return g_cleared.has(path) && g_cleared[path].integer(-1) == (int64_t)size;
}

bool Downloads::contains(const std::string& url) const {
    std::lock_guard<std::mutex> l(g_lock);
    return find(url) != nullptr;
}

std::vector<Job> Downloads::snapshot() const {
    std::lock_guard<std::mutex> l(g_lock);
    return g_jobs;
}

int Downloads::active() const {
    std::lock_guard<std::mutex> l(g_lock);
    int n = 0;
    for (const auto& j : g_jobs)
        if (j.state == JobState::Queued || j.state == JobState::Downloading) ++n;
    return n;
}

Downloads::FileState Downloads::file_state(const Item& item, const std::string& selected) {
    const std::string path = selected.empty() ? package_path(item) : selected;
    if (path.empty()) return FileState::None;
    if (lib::file_exists(path)) {
        if (!is_package_file(path) || (item.size && lib::file_size(path) != item.size)) return FileState::Partial;
        return FileState::Complete;
    }
    if (lib::file_exists(path + ".part") || lib::file_exists(path + ".resume")) return FileState::Partial;
    return FileState::None;
}

std::vector<std::string> Downloads::tick() {
    std::vector<std::string> messages;
    std::vector<Job> finished;
    {
        std::lock_guard<std::mutex> l(g_lock);
        while (!g_finished.empty()) {
            if (Job* j = by_id(g_finished.front())) finished.push_back(*j);
            g_finished.pop_front();
        }
    }
    for (const Job& job : finished) {
        if (!catalog()->config().install_after) {
            messages.push_back(job.item.name + " is downloaded to " + job.path);
            continue;
        }
        // Without the console's install service nothing can be installed from
        // here: the package is already where Package Installer lists it.
        if (!installer_ready()) {
            messages.push_back(job.item.name + " is downloaded. Install it from Debug settings > Package installer.");
            set_install_status(job.path, DownloadInstallState::Failed, "Automatic installation is unavailable. Downloaded PKG kept.");
            continue;
        }
        const InstallResult r = install_local(job.path, job.item);
        messages.push_back(job.item.name + " downloaded successfully. " +
            (r.ok ? r.message : "Automatic installation failed: " + r.message + " Downloaded PKG kept; retry Install."));
    }
    return messages;
}

}  // namespace store
