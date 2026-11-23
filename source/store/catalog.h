// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The content catalogue: FPKGi's own files, read as FPKGi reads them.
//
//   <data>/config.json            preferences, and a URL per category
//   <data>/ContentJSONs/GAMES.json one file per category:
//       { "DATA": { "<pkg url>": { "title_id", "region", "name", "version",
//                                  "release", "size", "min_fw", "cover_url" } } }
//
// When "Populate via web" is on and a category has a URL in config.json, that
// URL's JSON is used instead of the local file (and a copy is kept beside it
// as <FILE>.web.json for when the network is down).
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "core/json.h"

namespace store {

enum class Category { PS1, PS2, PSP, PS5, Games, Apps, Updates, DLC, Demos, Homebrew, Emulators, Themes, Count };

const char* category_name(Category c);       // "Games"
const char* category_file(Category c);       // "GAMES.json"
const char* category_config_key(Category c); // "games" (CONTENT_URLS in config.json)

struct Item {
    std::string url;  // where the .pkg is
    std::string title_id, region, name, version, release, min_fw, cover_url;
    std::string release_date, description, developer, publisher, genres, players, languages, rating;
    std::string release_date_scope, metadata_source;
    std::string latest_release_date;
    std::string homebrew_category = "Other";
    uint64_t size = 0;
    Category category = Category::Games;
    // A PS5 package (PPSA title id or "PS5" in its name/URL): drawn in a PS5 box.
    bool ps5 = false;
    // "owner/repo" for latest-download links and homebrew release links.
    // Latest-download links resolve version/size; homebrew also gets the
    // project's latest publication date without changing pinned packages.
    std::string github_repo;
    std::string github_asset;
};

// FPKGi's preferences (config.json), the parts FPKGi+ uses. Unknown keys in
// the file are kept when it is saved.
// Where downloads are saved on the console: the folder its Package Installer
// lists (/data/pkg), so a finished package is ready to install with nothing
// to move. (On a USB drive that is the drive's top folder.)
std::string console_download_path();
std::vector<std::string> connected_usb_drives();
const std::vector<std::string>& homebrew_categories();

struct Config {
    // Where downloads are saved: console_download_path() or a USB drive's top
    // folder ("/mnt/usb0/"). Nothing else: any other folder in config.json is
    // read as the console.
    std::string download_path;
    bool download_to_usb = false;
    bool direct_download = true;  // true: download here, then install; false: the console downloads it
    bool install_after = false;
    bool cleanup_after_install = false;
    bool skip_game_update_checks = true;
    // Downloads run in the background downloader (store/service.h) when it
    // can be used. FPKGIPLUS.backgroundDownloads; read when FPKGi+ starts.
    bool background_downloads = true;
    bool populate_via_web = false;
    bool debug_log = false;  // Settings: write every network request to the log
    int connections = 4;         // connections used at once for one download (1 to 8)
    std::string theme = "modern";  // "modern" or "arcade" (ui/theme.h)
    std::string homebrew_category = "All";
    std::string sort = "name";  // name, size, region, titleID
    bool ascending = true;
    std::vector<std::string> regions{"Asia", "Europe", "Japan", "USA"};
    std::string content_urls[(int)Category::Count];
};

// Resolve USB automatically when only one drive is connected. With multiple
// drives, keep an explicitly selected connected drive or request a choice.
bool resolve_download_destination(Config& config, const std::vector<std::string>& drives);

class Catalog {
public:
    // Reads config.json and every category (on a worker: may download).
    void load();
    bool loaded() const { return loaded_; }
    const std::vector<Item>& items(Category c) const { return items_[(int)c]; }
    size_t total() const;
    // Problems found while loading, for the screen ("GAMES.json: not valid JSON").
    const std::vector<std::string>& problems() const { return problems_; }

    Config& config() { return config_; }
    void save_config();
    // Reads config.json only (the background downloader needs the settings,
    // not the lists).
    void load_settings() { load_config(); }
    void update_game_metadata(const std::string& title_id, const lib::Json& fields);

private:
    void load_config();
    void resolve_latest_releases();
    void load_category(Category c);

    bool loaded_ = false;
    Config config_;
    lib::Json config_json_;
    std::vector<Item> items_[(int)Category::Count];
    std::vector<std::string> problems_;
};

// The catalogue the screens use. Replaced as a whole on reload (the UI thread
// swaps it in), so a screen never sees one half loaded.
std::shared_ptr<Catalog> catalog();
void set_catalog(std::shared_ptr<Catalog> c);

// Exposed for tests.
bool parse_content(const std::string& json, Category c, std::vector<Item>& out, std::string& problem);
std::string game_date(const std::string& value);  // validated ISO date or year; empty if unknown
void apply_metadata(Item& item, const lib::Json& fields);
std::string region_label(const std::string& region);  // "USA", "EUR", "JAP", "ASIA", "ALL", "???"
bool region_shown(const Config& config, const std::string& region);

}  // namespace store
