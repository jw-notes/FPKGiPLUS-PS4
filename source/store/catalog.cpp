// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "store/catalog.h"
#include "store/metadata.h"

#include <stdlib.h>
#include <time.h>

#include <algorithm>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "net/http.h"
#include "platform/platform.h"

namespace store {

namespace {

struct CategoryInfo {
    const char* name;
    const char* file;
    const char* key;
};
const CategoryInfo kCategories[] = {
    {"PS1", "PS1.json", "ps1"},          {"PS2", "PS2.json", "ps2"},          {"PSP", "PSP.json", "psp"},
    {"PS5", "PS5.json", "ps5"},          {"Games", "GAMES.json", "games"},    {"Apps", "APPS.json", "apps"},
    {"Updates", "UPDATES.json", "updates"}, {"DLC", "DLC.json", "dlc"},     {"Demos", "DEMOS.json", "demos"},
    {"Homebrew", "HOMEBREW.json", "homebrew"}, {"Emulators", "EMULATORS.json", "emulators"},
    {"Themes", "THEMES.json", "themes"},
};
// config.json writes some of these keys in capitals ("PS1", "DLC").
const char* kConfigSpelling[] = {"PS1", "PS2", "PSP", "PS5", "games", "apps", "updates", "DLC", "demos",
                                 "homebrew", "emulators", "themes"};

std::shared_ptr<Catalog> g_catalog = std::make_shared<Catalog>();

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

std::string base64_decode(const std::string& s) {
    std::string out;
    int bits = 0, value = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = b64_value(c);
        if (v < 0) return "";
        value = (value << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((value >> bits) & 0xFF));
        }
    }
    return out;
}

bool is_url(const std::string& s) {
    return lib::starts_with(lib::lower(s.substr(0, 8)), "http://") || lib::starts_with(lib::lower(s.substr(0, 8)), "https://");
}

// FPKGi ships one dummy entry per category pointing at a made-up host
// ("https://www.web.site/ps1-classic.pkg", an "example.com" URL and the like),
// with an invented multi-GB size. They are examples, not content: skip them so
// their categories do not turn into tabs full of junk.
bool is_placeholder(const std::string& url) {
    static const char* hosts[] = {"web.site",     "example.com", "example.org", "example.net",
                                  "your-website", "yourwebsite", "directlinkto", "www.site"};
    const std::string u = lib::lower(url);
    for (const char* h : hosts)
        if (lib::icontains(u, std::string("//") + h) || lib::icontains(u, std::string(".") + h)) return true;
    return false;
}

// FPKGi accepts keys written as Base64 of the URL; and it escapes spaces and
// non-ASCII characters that the JSON's author left as they are.
std::string proper_url(std::string url) {
    url = lib::trim(url);
    if (!url.empty() && !is_url(url) && url[0] != '/') {
        const std::string decoded = base64_decode(url);
        if (is_url(decoded)) url = decoded;
    }
    if (!is_url(url)) return url;  // a local file path stays as it is
    std::string out;
    for (size_t i = 0; i < url.size(); ++i) {
        const unsigned char c = (unsigned char)url[i];
        if (c == ' ') out += "%20";
        else if (c >= 0x80 || c < 0x20 || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' ||
                 c == '{' || c == '|' || c == '}') {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        } else {
            out += (char)c;
        }
    }
    return out;
}

uint64_t parse_size(const lib::Json& v) {
    if (v.type() == lib::Json::Type::Number) return v.num() > 0 ? (uint64_t)v.num() : 0;
    std::string s = lib::lower(lib::trim(v.str()));
    if (s.empty()) return 0;
    double n = strtod(s.c_str(), nullptr);
    if (n <= 0) return 0;
    if (lib::ends_with(s, "gb")) n *= 1024.0 * 1024 * 1024;
    else if (lib::ends_with(s, "mb")) n *= 1024.0 * 1024;
    else if (lib::ends_with(s, "kb")) n *= 1024.0;
    return (uint64_t)n;
}

std::string text_of(const lib::Json& v) {
    if (v.is_array()) {
        std::string out;
        for (const auto& entry : v.items()) {
            const std::string s = text_of(entry);
            if (!s.empty()) { if (!out.empty()) out += ", "; out += s; }
        }
        return out;
    }
    if (v.type() == lib::Json::Type::Number) {
        char buf[32];
        snprintf(buf, sizeof buf, "%g", v.num());
        return buf;
    }
    return lib::trim(v.str());
}

}  // namespace

std::string game_date(const std::string& value) {
    const std::string s = lib::trim(value);
    if (s.size() != 4 && s.size() != 7 && s.size() != 10) return "";
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) { if (s[i] != '-') return ""; }
        else if (s[i] < '0' || s[i] > '9') return "";
    }
    const int year = atoi(s.c_str());
    if (year < 1970 || year > 2199) return "";
    if (s.size() == 4) return s;
    const int month = atoi(s.c_str() + 5);
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return "";
    if (s.size() == 7) return s;
    const int day = atoi(s.c_str() + 8);
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day < 1 || day > days[month - 1] + (month == 2 && leap ? 1 : 0)) return "";
    return s;
}

void apply_metadata(Item& item, const lib::Json& v) {
    if (v.has("homebrew_category")) {
        item.homebrew_category = "Other";
        for (const auto& group : homebrew_categories())
            if (group != "All" && lib::iequals(group, v["homebrew_category"].str())) item.homebrew_category = group;
    }
    if (v.has("release_date")) {
        item.release_date = game_date(text_of(v["release_date"]));
        item.release_date_scope = text_of(v["release_date_scope"]);
    }
    if (v.has("metadata_source")) item.metadata_source = text_of(v["metadata_source"]);
    if (v.has("description")) item.description = lib::to_utf8(text_of(v["description"]));
    if (v.has("developer")) item.developer = text_of(v["developer"]);
    if (v.has("publisher")) item.publisher = text_of(v["publisher"]);
    if (v.has("genres")) item.genres = text_of(v["genres"]);
    if (v.has("players")) item.players = text_of(v["players"]);
    if (v.has("languages")) item.languages = text_of(v["languages"]);
    if (v.has("rating")) item.rating = text_of(v["rating"]);
}

const char* category_name(Category c) { return kCategories[(int)c].name; }
const char* category_file(Category c) { return kCategories[(int)c].file; }
const char* category_config_key(Category c) { return kCategories[(int)c].key; }

const std::vector<std::string>& homebrew_categories() {
    static const std::vector<std::string> groups{"All", "Utilities", "Media", "Emulators", "Games", "Other"};
    return groups;
}

std::string region_label(const std::string& region) {
    const std::string r = lib::lower(lib::trim(region));
    if (r == "usa" || r == "us") return "USA";
    if (r == "eur" || r == "eu" || r == "europe") return "EUR";
    if (r == "jap" || r == "jp" || r == "japan") return "JAP";
    if (r == "asia" || r == "as") return "ASIA";
    if (r == "all") return "ALL";
    return "???";
}

bool region_shown(const Config& config, const std::string& region) {
    const std::string r = region_label(region);
    auto has = [&](const char* name) {
        return std::find(config.regions.begin(), config.regions.end(), std::string(name)) != config.regions.end();
    };
    if (r == "USA") return has("USA");
    if (r == "EUR") return has("Europe");
    if (r == "JAP") return has("Japan");
    if (r == "ASIA") return has("Asia");
    return true;  // ALL and unknown regions are always shown, as in FPKGi
}

bool parse_content(const std::string& text, Category c, std::vector<Item>& out, std::string& problem) {
    bool ok = false;
    // Some files are saved with a byte-order mark.
    const std::string body = lib::starts_with(text, "\xEF\xBB\xBF") ? text.substr(3) : text;
    const lib::Json root = lib::Json::parse(body, &ok);
    if (!ok) {
        problem = "not valid JSON";
        return false;
    }
    const lib::Json& data = root["DATA"];
    if (!data.is_object()) {
        problem = "no \"DATA\" object";
        return false;
    }
    size_t skipped = 0;
    for (const auto& [key, v] : data.members()) {
        Item item;
        item.url = proper_url(key);
        item.title_id = lib::trim(v["title_id"].str());
        item.name = lib::collapse_space(lib::to_utf8(v["name"].str()));
        item.size = parse_size(v["size"]);
        // FPKGi drops entries without a title id, name or size; so do we, and
        // FPKGi's own dummy "example" rows as well.
        if (item.url.empty() || item.title_id.empty() || item.name.empty() || item.size == 0 ||
            is_placeholder(item.url)) {
            ++skipped;
            continue;
        }
        item.region = region_label(v["region"].str());
        item.version = text_of(v["version"]);
        item.release = text_of(v["release"]);
        apply_metadata(item, v);
        item.min_fw = text_of(v["min_fw"]);
        item.cover_url = v["cover_url"].is_null() ? "" : proper_url(v["cover_url"].str());
        item.category = c;
        item.ps5 = c == Category::PS5 || lib::starts_with(item.title_id, "PPSA") ||
                   lib::icontains(item.name, "PS5") || lib::icontains(key, "/PS5");
        // github.com/<owner>/<repo>/releases/... -> resolve the latest release.
        {
            const std::string u = item.url;
            size_t gh = u.find("github.com/");
            if (lib::starts_with(u, "https://github.com/") && gh != std::string::npos &&
                (u.find("/releases/latest/download/") != std::string::npos ||
                 (c == Category::Homebrew && u.find("/releases/download/") != std::string::npos))) {
                std::vector<std::string> parts = lib::split(u.substr(gh + 11), '/');
                if (parts.size() >= 2) {
                    item.github_repo = parts[0] + "/" + parts[1];
                    item.github_asset = parts.back();
                }
            }
        }
        out.push_back(std::move(item));
    }
    if (skipped) problem = std::to_string(skipped) + " entries without a title ID, name or size were skipped";
    return true;
}

void Catalog::load_config() {
    config_.download_path = console_download_path();
    config_.download_to_usb = false;
    const std::string path = lib::join_path(platform::data_root(), "config.json");
    std::string text;
    if (!lib::read_file(path, text)) {
        LOG("catalog: no config.json; using standalone defaults");
        config_json_ = lib::Json::object();
        return;
    }
    bool ok = false;
    config_json_ = lib::Json::parse(text, &ok);
    if (!ok || !config_json_.is_object()) {
        problems_.push_back("config.json: not valid JSON, using defaults");
        config_json_ = lib::Json::object();
        return;
    }
    const lib::Json& filtering = config_json_["FILTERING"];
    const lib::Json& prefs = config_json_["PREFERENCES"];
    const lib::Json& dl = prefs["DOWNLOADS"];
    const lib::Json& app = prefs["APPLICATION"];
    config_.direct_download = true;
    config_.install_after = dl["installAfter"].boolean(false);
    config_.cleanup_after_install = dl["cleanupAfterInstall"].boolean(false);
    const std::string path_value = lib::trim(dl["downloadPath"].str());
    // The console or a USB drive, nothing in between.
    if (lib::starts_with(path_value, "/mnt/usb") && path_value.size() >= 9 && path_value[8] >= '0' && path_value[8] <= '7' &&
        (path_value.size() == 9 || path_value[9] == '/'))
        config_.download_path = path_value.substr(0, 9) + "/";
    config_.download_to_usb = dl["downloadToUsb"].boolean(config_.download_path != console_download_path());
    if (config_.download_to_usb && config_.download_path == console_download_path()) config_.download_path.clear();
    if (!config_.download_to_usb) config_.download_path = console_download_path();
    config_.populate_via_web = app["populateViaWeb"].boolean(false);
    const std::string sort = lib::lower(filtering["SORT"]["type"].str());
    if (sort == "size" || sort == "region" || sort == "name" || sort == "titleid") config_.sort = sort;
    config_.ascending = filtering["SORT"]["ascending"].boolean(true);
    if (filtering["REGIONS"].is_array()) {
        config_.regions.clear();
        for (const auto& r : filtering["REGIONS"].items()) config_.regions.push_back(r.str());
    }
    const lib::Json& urls = prefs["CONTENT_URLS"];
    for (int i = 0; i < (int)Category::Count; ++i) {
        std::string url = urls[kConfigSpelling[i]].str();
        if (url.empty()) url = urls[kCategories[i].key].str();
        config_.content_urls[i] = url.empty() ? "" : proper_url(url);
    }
    // FPKGi+'s own settings, under a key FPKGi leaves alone.
    const lib::Json& plus = config_json_["FPKGIPLUS"];
    config_.skip_game_update_checks = plus["skipGameUpdateChecks"].boolean(true);
    config_.background_downloads = plus["backgroundDownloads"].boolean(true);
    config_.connections = (int)std::max<int64_t>(1, std::min<int64_t>(8, plus["connections"].integer(4)));
    config_.theme = plus["theme"].str() == "arcade" ? "arcade" : "modern";
    config_.homebrew_category = "All";
    for (const auto& group : homebrew_categories())
        if (group == plus["homebrewCategory"].str()) config_.homebrew_category = group;
    config_.debug_log = plus["debugLog"].boolean(false);
}

void Catalog::save_config() {
    // Only the values FPKGi+ changes are written; the rest of the file stays.
    lib::Json root = config_json_.is_object() ? config_json_ : lib::Json::object();
    lib::Json filtering = root["FILTERING"].is_object() ? root["FILTERING"] : lib::Json::object();
    lib::Json sort = filtering["SORT"].is_object() ? filtering["SORT"] : lib::Json::object();
    sort.set("type", config_.sort == "titleid" ? std::string("titleID") : config_.sort);
    sort.set("ascending", config_.ascending);
    filtering.set("SORT", sort);
    lib::Json regions = lib::Json::array();
    for (const auto& r : config_.regions) regions.push(r);
    filtering.set("REGIONS", regions);
    root.set("FILTERING", filtering);
    lib::Json prefs = root["PREFERENCES"].is_object() ? root["PREFERENCES"] : lib::Json::object();
    lib::Json dl = prefs["DOWNLOADS"].is_object() ? prefs["DOWNLOADS"] : lib::Json::object();
    dl.set("directDownload", config_.direct_download);
    dl.set("downloadPath", config_.download_path);
    dl.set("downloadToUsb", config_.download_to_usb);
    dl.set("installAfter", config_.install_after);
    dl.set("cleanupAfterInstall", config_.cleanup_after_install);
    prefs.set("DOWNLOADS", dl);
    lib::Json app = prefs["APPLICATION"].is_object() ? prefs["APPLICATION"] : lib::Json::object();
    app.set("populateViaWeb", config_.populate_via_web);
    // FPKGi's menu music stays off.
    app.set("backgroundMusic", false);
    prefs.set("APPLICATION", app);
    lib::Json urls = lib::Json::object();
    for (int i = 0; i < (int)Category::Count; ++i) urls.set(kConfigSpelling[i], config_.content_urls[i]);
    prefs.set("CONTENT_URLS", urls);
    root.set("PREFERENCES", prefs);
    lib::Json plus = root["FPKGIPLUS"].is_object() ? root["FPKGIPLUS"] : lib::Json::object();
    plus.erase("installPath");  // an older build's setting
    plus.set("debugLog", config_.debug_log);
    plus.set("connections", config_.connections);
    plus.set("theme", config_.theme);
    plus.set("homebrewCategory", config_.homebrew_category);
    plus.set("skipGameUpdateChecks", config_.skip_game_update_checks);
    plus.set("backgroundDownloads", config_.background_downloads);
    root.set("FPKGIPLUS", plus);
    config_json_ = root;
    const std::string path = lib::join_path(platform::data_root(), "config.json");
    if (!lib::write_file_atomic(path, root.dump(true))) LOG("catalog: could not save %s", path.c_str());
}

void Catalog::load_category(Category c) {
    // FPKGi's format has a PS5 list; a PS4 cannot install those packages.
    // The category stays in config.json so FPKGi settings round-trip.
    if (c == Category::PS5) return;
    const std::string dir = lib::join_path(platform::data_root(), "ContentJSONs");
    const std::string file = lib::join_path(dir, category_file(c));
    const std::string web_copy = file.substr(0, file.size() - 5) + ".web.json";
    std::string text, problem;
    const std::string& url = config_.content_urls[(int)c];
    bool from_web = false;
    if (config_.populate_via_web && !url.empty()) {
        lib::HttpRequest request;
        request.url = url;
        request.max_bytes = 64ull * 1024 * 1024;
        request.timeout_seconds = 20;
        const lib::HttpResponse response = lib::http_request(request);
        if (response.ok()) {
            text = response.body;
            from_web = true;
            lib::write_file_atomic(web_copy, text);
        } else {
            problems_.push_back(std::string(category_name(c)) + ": " + lib::describe_failure(response));
            lib::read_file(web_copy, text);  // the last copy, if there is one
        }
    }
    if (text.empty() && !lib::read_file(file, text)) {
        // Homebrew falls back to the open-source list that ships with the app;
        // any other category without a file is empty.
        if (c != Category::Homebrew ||
            !lib::read_file(lib::join_path(platform::assets_root(), "homebrew-list.json"), text))
            return;
        LOG("catalog: using the built-in homebrew list");
    }
    std::vector<Item>& out = items_[(int)c];
    if (!parse_content(text, c, out, problem)) {
        problems_.push_back(std::string(from_web ? url : category_file(c)) + ": " + problem);
        return;
    }
    if (!problem.empty()) LOG("catalog: %s: %s", category_file(c), problem.c_str());
    LOG("catalog: %s: %zu titles%s", category_name(c), out.size(), from_web ? " (from the web)" : "");
}

void Catalog::resolve_latest_releases() {
    // The last answers, so a version still shows when the API is unreachable or
    // rate-limited (unauthenticated GitHub allows 60 requests an hour).
    const std::string cache_path = lib::join_path(platform::data_root(), "FPKGIPLUS-releases.json");
    std::string cache_text;
    lib::read_file(cache_path, cache_text);
    bool ok = false;
    lib::Json cache = lib::Json::parse(cache_text, &ok);
    if (!ok || !cache.is_object()) cache = lib::Json::object();

    std::vector<std::string> repos;
    for (const auto& cat : items_)
        for (const auto& it : cat)
            if (!it.github_repo.empty() &&
                std::find(repos.begin(), repos.end(), it.github_repo) == repos.end())
                repos.push_back(it.github_repo);

    const int64_t now = (int64_t)time(nullptr);
    const int64_t kTtl = 6 * 3600;  // re-check a repo at most every six hours
    for (const std::string& repo : repos) {
        const int64_t checked = (int64_t)cache[repo]["checked"].integer(0);
        if (now > 0 && checked > 0 && now - checked < kTtl && cache[repo].has("published_at")) continue;
        lib::HttpRequest request;
        request.url = "https://api.github.com/repos/" + repo + "/releases/latest";
        request.timeout_seconds = 8;
        request.max_bytes = 2 * 1024 * 1024;
        const lib::HttpResponse r = lib::http_request(request);
        if (r.ok()) {
            bool parsed = false;
            lib::Json rel = lib::Json::parse(r.body, &parsed);
            if (parsed && rel.is_object()) {
                lib::Json entry = lib::Json::object();
                std::string tag = rel["tag_name"].str();
                if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag = tag.substr(1);
                entry.set("version", tag);
                // GitHub's publication date belongs to the release; the
                // creation/update timestamps can refer to unrelated edits.
                entry.set("published_at", rel["published_at"].str());
                lib::Json sizes = lib::Json::object();
                lib::Json urls = lib::Json::object();
                for (const auto& a : rel["assets"].items()) {
                    sizes.set(a["name"].str(), (double)a["size"].integer(0));
                    urls.set(a["name"].str(), a["browser_download_url"].str());
                }
                entry.set("assets", sizes);
                entry.set("urls", urls);
                entry.set("checked", (double)now);
                cache.set(repo, entry);
            }
        } else {
            LOG("catalog: latest release for %s: %s", repo.c_str(), lib::describe_failure(r).c_str());
        }
    }
    lib::write_file_atomic(cache_path, cache.dump(true));

    // Apply what we have (fresh or cached) to every GitHub-backed item.
    for (auto& cat : items_)
        for (auto& it : cat) {
            if (it.github_repo.empty()) continue;
            const lib::Json& entry = cache[it.github_repo];
            if (it.category == Category::Homebrew)
                it.latest_release_date = game_date(entry["published_at"].str().substr(0, 10));
            // A pinned package keeps its own version and download URL while
            // its explicitly labelled latest-release date is shown separately.
            if (it.url.find("/releases/latest/download/") == std::string::npos) continue;
            const std::string url = entry["urls"][it.github_asset].str();
            if (url.empty()) continue;  // never relabel a pinned or unavailable asset
            it.url = proper_url(url);
            const std::string v = entry["version"].str();
            if (!v.empty()) it.version = v;
            const uint64_t size = (uint64_t)entry["assets"][it.github_asset].integer(0);
            if (size > 0) it.size = size;
        }
}

std::string console_download_path() {
#ifdef LIB_HOST
    return lib::join_path(platform::data_root(), "Downloads/");
#else
    return "/data/pkg/";
#endif
}

std::vector<std::string> connected_usb_drives() {
    std::vector<std::string> drives;
    for (const auto& slot : platform::usb_mount_points()) {
        std::vector<lib::DirEntry> entries;
        // Like Librarian: unused PS4 mount slots also exist, so an
        // openable directory alone does not identify a connected drive.
        if (lib::list_dir(slot, entries, false) && !entries.empty())
            drives.push_back(lib::ends_with(slot, "/") ? slot : slot + "/");
    }
    return drives;
}

bool resolve_download_destination(Config& config, const std::vector<std::string>& drives) {
    if (!config.download_to_usb) {
        config.download_path = console_download_path();
        return true;
    }
    if (drives.size() == 1) {
        config.download_path = drives.front();
        return true;
    }
    if (!drives.empty() && std::find(drives.begin(), drives.end(), config.download_path) != drives.end()) return true;
    config.download_path.clear();
    return false;
}

void Catalog::load() {
    load_config();
    if (config_.download_to_usb) resolve_download_destination(config_, connected_usb_drives());
#ifndef LIB_HOST
    {
        // Downloads used to be kept in the app's own folder and moved when they
        // finished: what is still there (partial files too) moves once to
        // where downloads are saved now. Same disk: each is an instant rename.
        const std::string old_dir = lib::join_path(platform::data_root(), "Downloads");
        std::vector<lib::DirEntry> left;
        if (lib::list_dir(old_dir, left, false)) {
            lib::make_dirs(console_download_path());
            for (const auto& e : left) {
                if (e.is_dir) continue;
                const std::string to = console_download_path() + e.name;
                if (!lib::file_exists(to) && lib::rename_file(lib::join_path(old_dir, e.name), to))
                    LOG("downloads: moved %s to %s", e.name.c_str(), console_download_path().c_str());
            }
        }
    }
#endif
#ifndef LIB_HOST
    // One-time content-only migration. Never modify or depend on the old app's config.
    const std::string marker = lib::join_path(platform::data_root(), "content-imported");
    if (!lib::file_exists(marker)) {
        const std::string dest = lib::join_path(platform::data_root(), "ContentJSONs");
        lib::make_dirs(dest);
        for (int i = 0; i < (int)Category::Count; ++i) {
            const std::string target = lib::join_path(dest, category_file((Category)i));
            if (lib::file_exists(target)) continue;
            for (const char* old_root : {"/data/FPKGi/ContentJSONs", "/user/data/FPKGi/ContentJSONs"}) {
                std::string body;
                if (lib::read_file(lib::join_path(old_root, category_file((Category)i)), body) &&
                    lib::write_file_atomic(target, body)) break;
            }
        }
        lib::write_file_atomic(marker, "Content lists copied once; FPKGi is not required.\n");
    }
#endif
    for (int i = 0; i < (int)Category::Count; ++i) load_category((Category)i);
    resolve_latest_releases();
    // Project descriptions and maintainer credits ship with the app and work
    // offline. Match both the title ID and project URL to avoid misattribution.
    std::string homebrew_info;
    if (lib::read_file(lib::join_path(platform::assets_root(), "homebrew-metadata.json"), homebrew_info)) {
        bool ok = false;
        const auto info = lib::Json::parse(homebrew_info, &ok);
        if (ok && info.is_object()) for (auto& item : items_[(int)Category::Homebrew]) {
            const auto& fields = info[item.title_id];
            const std::string project = fields["metadata_source"].str();
            if (!project.empty() && lib::starts_with(item.url, project + "/")) {
                if (item.description.empty()) item.description = fields["description"].str();
                if (item.developer.empty()) item.developer = fields["developer"].str();
                if (item.metadata_source.empty()) item.metadata_source = project;
                if (item.homebrew_category == "Other" && fields.has("homebrew_category"))
                    item.homebrew_category = fields["homebrew_category"].str("Other");
            }
        }
    }
    std::string automatic;
    if (lib::read_file(lib::join_path(platform::data_root(), "wikidata-metadata.json"), automatic)) {
        bool ok = false;
        const auto root = lib::Json::parse(automatic, &ok);
        if (ok && root.is_object()) for (auto& category : items_) for (auto& item : category)
            apply_metadata(item, root[item.title_id]);
    }
    std::string metadata;
    if (lib::read_file(lib::join_path(platform::data_root(), "metadata.json"), metadata)) {
        bool ok = false;
        const lib::Json root = lib::Json::parse(metadata, &ok);
        if (!ok || !root.is_object()) problems_.push_back("metadata.json: not a JSON object");
        else for (auto& category : items_) for (auto& item : category)
            apply_metadata(item, root[item.title_id]);
    }
    if (!lib::file_exists(lib::join_path(platform::data_root(), "config.json"))) save_config();
    loaded_ = true;
}

size_t Catalog::total() const {
    size_t n = 0;
    for (const auto& v : items_) n += v.size();
    return n;
}

std::shared_ptr<Catalog> catalog() { return g_catalog; }
void Catalog::update_game_metadata(const std::string& title_id, const lib::Json& fields) {
    for (auto& category : items_) for (auto& item : category)
        if (item.title_id == title_id) apply_game_metadata(item, fields);
}
void set_catalog(std::shared_ptr<Catalog> c) { g_catalog = std::move(c); }

}  // namespace store
