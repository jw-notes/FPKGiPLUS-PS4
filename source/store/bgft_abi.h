// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace store::bgft {
// The SDK incorrectly declares packageSize and the progress lengths as 32 bit.
// Match the native 64-bit ABI used by Itemzflow's storage installer.
struct Parameters {
    int32_t userId, entitlementType;
    const char *id, *contentUrl, *contentExUrl, *contentName, *iconPath, *skuId;
    uint32_t option;
    const char *playgoScenarioId, *releaseDate, *packageType, *packageSubType;
    uint64_t packageSize;
};
struct StorageParameters { Parameters params; uint32_t slot; };
struct Progress {
    uint32_t bits; int32_t error;
    uint64_t length, transferred, length_total, transferred_total;
    uint32_t index, count, rest_seconds, rest_total;
    int32_t preparing_percent, local_copy_percent;
};
static_assert(sizeof(Parameters) == 104 && offsetof(Parameters, packageSize) == 96, "BGFT parameter ABI");
static_assert(sizeof(StorageParameters) == 112 && offsetof(StorageParameters, slot) == 104, "BGFT storage parameter ABI");
static_assert(sizeof(Progress) == 64 && offsetof(Progress, local_copy_percent) == 60, "BGFT progress ABI");

inline std::string storage_path(const std::string& file) {
    // /data is an app-visible alias. BGFT rejects it as a content_url; the
    // console Package Installer registers the physical /user/data path.
    if (file == "/data") return "/user/data";
    if (file.compare(0, 6, "/data/") == 0) return "/user/data/" + file.substr(6);
    return file;
}

inline StorageParameters local_parameters(const char* file, const char* name, const char* icon) {
    StorageParameters result{};
    result.params.entitlementType = 5;
    result.params.id = "";
    result.params.contentUrl = file;
    result.params.contentName = name;
    // Storage installs take a local icon path, not a remote cover URL. Use
    // Itemzflow's local placeholder when no local artwork is available.
    result.params.iconPath = icon && icon[0] == '/' ? icon : "/update/fakepic.png";
    result.params.playgoScenarioId = "0";
    result.params.option = 0x10000; // DISABLE_CDN_QUERY_PARAM
    // The storage service derives user, content ID, package type and size
    // from the local file. Remote-registration metadata remains unset.
    return result;
}
}
