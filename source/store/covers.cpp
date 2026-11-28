// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "store/covers.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "gfx/image.h"
#include "gfx/image_cache.h"
#include "net/http.h"
#include "platform/platform.h"
#include "store/installer.h"

namespace store {

namespace {

constexpr const char* kPrefix = "art:";
constexpr const char* kAppIcon = "appicon:";
// Covers are drawn at most about 420 logical pixels wide (the details page);
// 512 keeps them sharp there and small on disk (about 40 KB each).
constexpr int kCachedSize = 512;

std::string covers_dir() { return lib::join_path(platform::data_root(), "Covers"); }

constexpr const char* kShotPrefix = "shot:";
// Store screenshots are up to 4K (several MB each): kept at the screen's size.
constexpr int kShotWidth = 1920, kShotHeight = 1080;
std::string shots_dir() { return lib::join_path(platform::data_root(), "Screenshots"); }

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

// A cover from the web or from a file.
bool fetch(const std::string& url, std::string& bytes) {
    if (lib::starts_with(url, "http://") || lib::starts_with(url, "https://")) {
        lib::HttpRequest request;
        request.url = url;
        request.max_bytes = 16 * 1024 * 1024;
        request.timeout_seconds = 20;
        lib::HttpResponse r = lib::http_request(request);
        if (!r.ok()) return false;
        bytes = std::move(r.body);
        return !bytes.empty();
    }
    return lib::read_file(url, bytes, 16 * 1024 * 1024) && !bytes.empty();
}

// icon0.png out of a package on the web, with range requests.
bool remote_package_icon(const std::string& package_url, std::string& png) {
    std::string url = package_url;  // after the first answer: where the redirects ended
    std::string start;              // the first 64 KB: the header, the entry table and often the icon
    const auto range = [&](uint64_t offset, uint64_t length, std::string& out) {
        lib::HttpRequest request;
        request.url = url;
        request.headers.emplace_back("Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1));
        request.max_bytes = length;
        request.timeout_seconds = 20;
        lib::HttpResponse r = lib::http_request(request);
        if (r.status != 206) return false;  // the server ignores ranges: not worth the whole package
        if (!r.final_url.empty()) url = r.final_url;
        out = std::move(r.body);
        return true;
    };
    const uint64_t kStart = 64 * 1024;
    return read_package_icon([&](uint64_t offset, uint64_t length, std::string& out) {
        if (length == 0) return false;
        if (start.empty() && offset == 0 && !range(0, kStart, start)) return false;
        if (offset + length <= start.size()) { out = start.substr(offset, length); return true; }
        return range(offset, length, out);
    }, png);
}

// "appicon:<title id>\x01<version>\x01<package url>\x01<cover url>"
bool load_app_icon(const std::string& key, std::string& bytes) {
    std::vector<std::string> parts;
    size_t from = strlen(kAppIcon);
    for (int i = 0; i < 3; ++i) {
        const size_t cut = key.find('\x01', from);
        if (cut == std::string::npos) return false;
        parts.push_back(key.substr(from, cut - from));
        from = cut + 1;
    }
    parts.push_back(key.substr(from));
    // The icon comes from the package itself, never from what is installed
    // on the console (a catalogue's /user/appmeta cover is not used either).
    const std::string& package = parts[2];
    if ((lib::starts_with(package, "http://") || lib::starts_with(package, "https://")) && remote_package_icon(package, bytes))
        return true;
    const std::string& cover = parts[3];
    return (lib::starts_with(cover, "http://") || lib::starts_with(cover, "https://")) && fetch(cover, bytes);
}

bool load(const std::string& source, std::string& bytes) {
    const std::string url = source.substr(4);
    const std::string cached = cover_cache_path(url);
    if (lib::read_file(cached, bytes, 8 * 1024 * 1024) && !bytes.empty()) return true;
    if (lib::starts_with(url, kAppIcon) ? !load_app_icon(url, bytes) : !fetch(url, bytes)) return false;
    // Keep a small JPEG copy; the original bytes are decoded this time.
    gfx::Image image;
    if (gfx::decode_image(bytes, image, kCachedSize, kCachedSize)) {
        std::string jpeg;
        if (gfx::encode_jpeg(image, 88, jpeg)) {
            lib::make_dirs(covers_dir());
            lib::write_file_atomic(cached, jpeg);
            bytes = std::move(jpeg);
        }
    }
    return true;
}

bool load_shot(const std::string& source, std::string& bytes) {
    const std::string url = source.substr(strlen(kShotPrefix));
    char name[32];
    snprintf(name, sizeof name, "%016llx.jpg", (unsigned long long)fnv1a(url));
    const std::string cached = lib::join_path(shots_dir(), name);
    if (lib::read_file(cached, bytes, 16 * 1024 * 1024) && !bytes.empty()) return true;
    // The store's pictures are 4K (up to about 13 MB); its image service
    // sends the same picture as a JPEG at the screen's width (about 300 KB).
    // (Plain HTTP, like the pictures themselves: see store/metadata.cpp.)
    std::string path;
    for (const char* vulcan : {"http://vulcan.dl.playstation.net/", "https://vulcan.dl.playstation.net/"})
        if (lib::starts_with(url, vulcan)) path = url.substr(strlen(vulcan));
    const bool resized = !path.empty() &&
        fetch("http://image.api.playstation.com/vulcan/" + path + "?w=" + std::to_string(kShotWidth), bytes);
    if (!resized && !fetch(url, bytes)) return false;
    gfx::Image image;
    // decode_image takes a 4K picture one at a time by itself.
    if (gfx::decode_image(bytes, image, kShotWidth, kShotHeight)) {
        std::string jpeg;
        if (gfx::encode_jpeg(image, 85, jpeg)) {
            lib::make_dirs(shots_dir());
            lib::write_file_atomic(cached, jpeg);
            bytes = std::move(jpeg);
        }
    }
    return true;
}

}  // namespace

std::string screenshot_source(const std::string& url) { return url.empty() ? "" : kShotPrefix + url; }

std::string cover_cache_path(const std::string& cover_url) {
    char name[32];
    snprintf(name, sizeof name, "%016llx.jpg", (unsigned long long)fnv1a(cover_url));
    return lib::join_path(covers_dir(), name);
}

void covers_init() {
    lib::make_dirs(covers_dir());
    gfx::image_register_prefix_loader(kPrefix, load);
    gfx::image_register_prefix_loader(kShotPrefix, load_shot);
}

std::string cover_source(const std::string& cover_url) { return cover_url.empty() ? "" : kPrefix + cover_url; }

std::string cover_source(const Item& item) {
    if (item.category != Category::Homebrew || item.title_id.empty()) return cover_source(item.cover_url);
    return std::string(kPrefix) + kAppIcon + item.title_id + '\x01' + item.version + '\x01' + item.url + '\x01' + item.cover_url;
}

}  // namespace store
