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

}  // namespace

std::string cover_cache_path(const std::string& cover_url) {
    char name[32];
    snprintf(name, sizeof name, "%016llx.jpg", (unsigned long long)fnv1a(cover_url));
    return lib::join_path(covers_dir(), name);
}

void covers_init() {
    lib::make_dirs(covers_dir());
    gfx::image_register_prefix_loader(kPrefix, load);
}

std::string cover_source(const std::string& cover_url) { return cover_url.empty() ? "" : kPrefix + cover_url; }

std::string cover_source(const Item& item) {
    if (item.category != Category::Homebrew || item.title_id.empty()) return cover_source(item.cover_url);
    return std::string(kPrefix) + kAppIcon + item.title_id + '\x01' + item.version + '\x01' + item.url + '\x01' + item.cover_url;
}

}  // namespace store
