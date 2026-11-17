// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The host has no install service: an install is written to
// <data>/system/installs.txt and the title's folder is made under
// <data>/system/app (or patch / addcont), so the screens show it installed.
#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "store/installer.h"
#include <map>

namespace store {

bool installer_init() { return true; }
bool installer_ready() { return true; }
std::string installer_problem() { return ""; }
bool can_install_from_url() { return true; }
bool query_app_registered(const std::string&, bool&) { return false; }

static InstallResult mark_installed(const Item& item, const std::string& source) {
    const std::string root = system_root();
    const char* sub = item.category == Category::Updates ? "patch" : item.category == Category::DLC ? "addcont" : "app";
    const std::string dir = lib::join_path(lib::join_path(root, sub), item.title_id);
    lib::make_dirs(dir);
    if (item.category != Category::DLC) {
        std::string bytes;
        if (!lib::read_file(source, bytes, 16 << 20)) bytes = "\x7f" "CNT";
        lib::write_file_atomic(lib::join_path(dir, std::string(sub) + ".pkg"), bytes);
    }
    std::string log;
    lib::read_file(lib::join_path(root, "installs.txt"), log);
    log += item.title_id + " " + source + "\n";
    lib::write_file_atomic(lib::join_path(root, "installs.txt"), log);
    LOG("installer (host): %s %s", item.title_id.c_str(), source.c_str());
    refresh_installed(true);
    InstallResult r;
    r.ok = true;
    r.message = "Installing " + item.name + " (host test).";
    return r;
}

InstallResult platform_install_local(const std::string& file, const Item& item) {
    if (!is_package_file(file)) {
        InstallResult r;
        r.message = "This file is not a PS4 package.";
        return r;
    }
    // Mirror the console: place it in the install folder, then mark installed.
    place_in_install_folder(file, item);
    auto result = mark_installed(item, file);
    static int32_t next_task = 1;
    result.task_id = next_task++;
    std::string sfo;
    FILE* pkg = lib::open_file(file, "rb");
    if (pkg) {
        const bool ok = read_package_entry([&](uint64_t at, uint64_t size, std::string& out) {
            if (fseeko(pkg, at, SEEK_SET) != 0) return false;
            out.resize(size); return fread(&out[0], 1, size, pkg) == size;
        }, 0x1000, sfo);
        fclose(pkg);
        if (ok) {
            const std::string path = lib::join_path(system_root(), "appmeta/" + item.title_id + "/param.sfo");
            lib::make_dirs(lib::parent_dir(path)); lib::write_file_atomic(path, sfo);
        }
    }
    return result;
}

namespace {
struct TaskResult { InstallTaskState state; bool releasable; int percent; };
std::map<int32_t, TaskResult> g_task_results;
}
void host_set_install_task_result(int32_t task, InstallTaskState state, bool releasable, int percent) { g_task_results[task] = {state, releasable, percent}; }
InstallTaskProgress query_install_task(int32_t task) {
    const auto found = g_task_results.find(task);
    const auto state = found == g_task_results.end() ? InstallTaskState::Complete : found->second.state;
    const int percent = state == InstallTaskState::Complete ? 100 : found->second.percent;
    return {state, "Simulated install result", percent, percent >= 0 ? (uint64_t)percent : 0, percent >= 0 ? 100u : 0u};
}
bool release_install_task(int32_t task, bool, std::string& problem) {
    const auto found = g_task_results.find(task);
    if (found != g_task_results.end() && !found->second.releasable) { problem = "Simulated task still in use"; return false; }
    g_task_results.erase(task); problem.clear(); return true;
}

InstallResult install_from_url(const std::string& url, const Item& item) {
    if (const std::string self = self_install_problem(url, item); !self.empty()) return {false, self};
    return mark_installed(item, url);
}

InstallResult launch_title(const Item& item) {
    LOG("launch (host): %s", item.title_id.c_str());
    InstallResult r;
    r.ok = true;
    r.message = "Starting " + item.name + " (host test).";
    return r;
}

InstallResult delete_app(const Item& item) {
    refresh_installed(true);
    InstallResult r;
    r.message = app_delete_problem(item);
    if (!r.message.empty()) return r;
    const std::string dir = lib::join_path(lib::join_path(system_root(), "app"), item.title_id);
    r.ok = lib::remove_file(lib::join_path(dir, "app.pkg")) && lib::remove_dir(dir);
    r.message = r.ok ? "Deleted " + item.name + " (host test)." : "Could not remove the simulated app.";
    refresh_installed(true);
    return r;
}

}  // namespace store
