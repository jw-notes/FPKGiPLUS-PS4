// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
#include "store/homebrew_updates.h"
#include <algorithm>
#include <cstdio>
#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "core/usb_ftp.h"
#include "net/http.h"

namespace store {
std::string sfo_string(const std::string& bytes, const std::string& key) {
    if (bytes.size() < 20 || bytes.compare(0, 4, std::string("\0PSF", 4)) != 0) return "";
    auto le = [&](size_t at, size_t n) {
        uint32_t value = 0;
        for (size_t i = 0; i < n; ++i) value |= (uint32_t)(unsigned char)bytes[at + i] << (8 * i);
        return value;
    };
    const uint32_t keys = le(8, 4), data = le(12, 4), count = le(16, 4);
    if (count > 1024 || 20ull + count * 16ull > bytes.size() || keys > data || data > bytes.size()) return "";
    for (size_t i = 0; i < count; ++i) {
        const size_t at = 20 + i * 16;
        const uint64_t name = (uint64_t)keys + le(at, 2), value = (uint64_t)data + le(at + 12, 4);
        const uint32_t length = le(at + 4, 4);
        if (name >= data || value > bytes.size() || length > bytes.size() - value) return "";
        const size_t end = bytes.find('\0', (size_t)name);
        if (end == std::string::npos || end >= data) return "";
        if (bytes.substr((size_t)name, end - name) != key) continue;
        if (le(at + 2, 2) != 0x0204 || length == 0) return "";
        const std::string text = bytes.substr((size_t)value, length);
        return lib::trim(text.substr(0, text.find('\0')));
    }
    return "";
}

int compare_app_versions(const std::string& left, const std::string& right) {
    auto parse = [](std::string text, std::vector<uint64_t>& out) {
        text = lib::trim(text);
        if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.erase(0, 1);
        if (text.empty() || text.size() > 64) return false;
        for (const auto& part : lib::split(text, '.')) {
            if (part.empty() || part.size() > 9 || part.find_first_not_of("0123456789") != std::string::npos) return false;
            uint64_t value = 0;
            for (char c : part) value = value * 10 + c - '0';
            out.push_back(value);
        }
        return !out.empty() && text.back() != '.';
    };
    std::vector<uint64_t> a, b;
    if (!parse(left, a) || !parse(right, b)) return 2;
    a.resize(std::max(a.size(), b.size()), 0); b.resize(a.size(), 0);
    return a < b ? -1 : a > b ? 1 : 0;
}

HomebrewUpdate inspect_homebrew_update(const Item& item, const lib::Json& release,
    const std::string& installed_sfo, const PackageReader& latest_package) {
    HomebrewUpdate result;
    result.item = item;
    result.installed_version = sfo_string(installed_sfo, "APP_VER");
    if (sfo_string(installed_sfo, "TITLE_ID") != item.title_id || result.installed_version.empty()) {
        result.problem = "Could not read the installed app's version. No update was queued.";
        return result;
    }
    if (release["draft"].boolean() || release["prerelease"].boolean()) {
        result.problem = "No stable release was found."; return result;
    }
    std::string latest_sfo;
    if (!read_package_entry(latest_package, 0x1000, latest_sfo) || sfo_string(latest_sfo, "TITLE_ID") != item.title_id) {
        result.problem = "Could not verify that the latest package belongs to this app. No update was queued.";
        return result;
    }
    result.latest_version = sfo_string(latest_sfo, "APP_VER");
    const int comparison = compare_app_versions(result.latest_version, result.installed_version);
    if (comparison == 2) {
        result.problem = "Could not compare the installed and latest package versions. No update was queued.";
        return result;
    }
    result.available = comparison == 1;
    result.item.version = result.latest_version;
    result.item.latest_release_date = game_date(release["published_at"].str().substr(0, 10));
    return result;
}

HomebrewUpdate check_homebrew_update(const Item& item, const std::string& downloaded_path) {
    HomebrewUpdate result;
    result.item = item;
    if (item.category != Category::Homebrew || item.github_repo.empty()) {
        result.problem = "This homebrew has no GitHub release source to check."; return result;
    }
    LOG("homebrew update: checking %s from %s", item.title_id.c_str(), item.github_repo.c_str());
    const std::string installed = installed_app_sfo(item);
    if (installed.empty()) {
        result.problem = "Could not read the installed app's version from its PKG. No update was queued.";
        LOG("homebrew update: %s: installed SFO unavailable", item.title_id.c_str());
        return result;
    }
    lib::HttpRequest request;
    request.url = "https://api.github.com/repos/" + item.github_repo + "/releases/latest";
    request.max_bytes = 2 << 20;
    request.headers.emplace_back("Cache-Control", "no-cache");
    const auto response = lib::http_request(request);
    if (!response.ok()) {
        if (response.status == 404) result.problem = "This project has no published latest release. No update was queued.";
        else if (response.status == 403 || response.status == 429)
            result.problem = "GitHub blocked or rate-limited the update check. Try again later.";
        else result.problem = "Update check: " + lib::describe_failure(response);
        LOG("homebrew update: %s: release HTTP %d", item.title_id.c_str(), response.status);
        return result;
    }
    bool ok = false;
    const auto release = lib::Json::parse(response.body, &ok);
    if (!ok || !release.is_object()) { result.problem = "The release service returned invalid data."; return result; }
    std::vector<lib::Json> packages, matching;
    for (const auto& asset : release["assets"].items()) {
        const std::string name = asset["name"].str();
        if (!lib::ends_with(lib::lower(name), ".pkg")) continue;
        packages.push_back(asset);
        if (name == item.github_asset || lib::icontains(name, item.title_id)) matching.push_back(asset);
    }
    if (matching.size() == 1) packages = matching;
    if (packages.size() != 1) { result.problem = "The latest release does not identify a single matching PS4 package."; return result; }
    Item update = item;
    update.url = packages.front()["browser_download_url"].str();
    const int64_t asset_size = packages.front()["size"].integer();
    update.size = asset_size > 0 ? (uint64_t)asset_size : 0;
    if (!lib::starts_with(update.url, "https://") || update.size == 0) {
        result.problem = "The latest release has no usable package download."; return result;
    }
    std::string url = update.url;
    std::string range_problem;
    const PackageReader read = [&](uint64_t offset, uint64_t length, std::string& bytes) {
        lib::HttpRequest range;
        range.url = url;
        range.max_bytes = length;
        range.headers.emplace_back("Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1));
        const auto response = lib::http_request(range);
        if (!response.ok() || response.status != 206 || response.body.size() != length) {
            range_problem = response.ok() ? "The package server did not return the requested byte range." : lib::describe_failure(response);
            LOG("homebrew update: %s: PKG range %llu+%llu, HTTP %d", item.title_id.c_str(),
                (unsigned long long)offset, (unsigned long long)length, response.status);
            return false;
        }
        // The body must be the requested bytes, rather than an ignored range.
        const std::string wanted = "bytes " + std::to_string(offset) + "-" + std::to_string(offset + length - 1) + "/";
        if (!lib::starts_with(response.content_range, wanted)) {
            range_problem = "The package server returned a different byte range.";
            return false;
        }
        bytes = response.body;
        if (!response.final_url.empty()) url = response.final_url;
        return true;
    };
    result = inspect_homebrew_update(update, release, installed, read);
    if (!range_problem.empty()) result.problem = "Latest PKG check: " + range_problem + " No update was queued.";
    LOG("homebrew update: %s: installed %s, latest %s, available %d, problem %s", item.title_id.c_str(),
        result.installed_version.c_str(), result.latest_version.c_str(), result.available, result.problem.c_str());
    if (result.available && !downloaded_path.empty()) {
        FILE* file = lib::open_file(downloaded_path, "rb");
        if (file) {
            std::string sfo;
            const PackageReader local = [&](uint64_t offset, uint64_t length, std::string& bytes) {
                if (fseeko(file, (off_t)offset, SEEK_SET) != 0) return false;
                bytes.resize((size_t)length);
                return fread(&bytes[0], 1, (size_t)length, file) == length;
            };
            result.downloaded = read_package_entry(local, 0x1000, sfo) &&
                sfo_string(sfo, "TITLE_ID") == item.title_id &&
                compare_app_versions(sfo_string(sfo, "APP_VER"), result.latest_version) == 0;
            fclose(file);
            if (result.downloaded) result.downloaded_path = downloaded_path;
        }
    }
    return result;
}
}
