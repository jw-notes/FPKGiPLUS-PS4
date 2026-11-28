#include "store/open_media.h"

#include <time.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "net/http.h"
#include "platform/platform.h"
#include "store/metadata.h"

namespace store {
namespace {

constexpr const char* kUserAgent = "FPKGiPlus (https://github.com/jw-notes/FPKGiPLUS-PS4)";
constexpr int64_t kIndexMaxAge = 30 * 24 * 3600;  // a libretro index is fetched again after 30 days

lib::Json get_json(const std::string& url) {
    lib::HttpRequest request;
    request.url = url;
    request.headers.emplace_back("User-Agent", kUserAgent);
    request.timeout_seconds = 20;
    request.max_bytes = 8 * 1024 * 1024;
    const auto response = lib::http_request(request);
    if (!response.ok()) {
        LOG("open media: %s: %s", url.c_str(), lib::describe_failure(response).c_str());
        return {};
    }
    bool ok = false;
    const auto root = lib::Json::parse(response.body, &ok);
    return ok ? root : lib::Json();
}

// Letters and digits only, lower case, without "the" and any (...) or [...]
// tags: "Getaway, The (Europe)" and "The Getaway" are the same.
std::string key_of(const std::string& name) {
    std::string plain;
    int depth = 0;
    for (char c : lib::decode_entities(name)) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (!depth) plain += c;
    }
    std::string spaced;
    for (unsigned char c : lib::lower(plain)) spaced += (std::isalnum(c) || c >= 128) ? (char)c : ' ';
    std::string out;
    for (const std::string& word : lib::split(lib::collapse_space(spaced), ' ')) {
        if (word.empty() || word == "the") continue;
        if (!out.empty()) out += ' ';
        out += word;
    }
    return out;
}

// ---- libretro-thumbnails (PS1, PS2, PSP) ---------------------------------------

const char* libretro_repo(const Item& item) {
    switch (item.category) {
        case Category::PS1: return "Sony_-_PlayStation";
        case Category::PS2: return "Sony_-_PlayStation_2";
        case Category::PSP: return "Sony_-_PlayStation_Portable";
        default: return nullptr;
    }
}

std::mutex g_index_lock;

// The picture names in one folder of a repository ("Named_Snaps"), kept in
// <data>/OpenMedia for 30 days: the first line is when it was fetched.
std::vector<std::string> libretro_index(const std::string& repo, const std::string& folder) {
    std::lock_guard<std::mutex> lock(g_index_lock);
    const std::string dir = lib::join_path(platform::data_root(), "OpenMedia");
    const std::string path = lib::join_path(dir, repo + "-" + folder + ".txt");
    std::string text;
    std::vector<std::string> names;
    if (lib::read_file(path, text, 8 * 1024 * 1024)) {
        names = lib::split(text, '\n');
        if (!names.empty() && time(nullptr) - atoll(names[0].c_str()) < kIndexMaxAge) {
            names.erase(names.begin());
            return names;
        }
    }
    // The repository's top folder names each picture folder's tree; that tree
    // lists its files (about 10,000 names, 2 MB).
    const std::string api = "https://api.github.com/repos/libretro-thumbnails/" + repo + "/git/trees/";
    std::string sha;
    const lib::Json top = get_json(api + "master");
    for (const auto& entry : top["tree"].items())
        if (entry["path"].str() == folder) sha = entry["sha"].str();
    if (sha.empty()) {
        if (!names.empty()) names.erase(names.begin());  // an old index beats none
        return names;
    }
    std::vector<std::string> fresh;
    const lib::Json listing = get_json(api + sha);
    for (const auto& entry : listing["tree"].items()) {
        const std::string file = entry["path"].str();
        if (file.size() > 4 && file.compare(file.size() - 4, 4, ".png") == 0) fresh.push_back(file.substr(0, file.size() - 4));
    }
    if (fresh.empty()) {
        if (!names.empty()) names.erase(names.begin());
        return names;
    }
    std::string out = std::to_string((long long)time(nullptr));
    for (const auto& name : fresh) out += "\n" + name;
    lib::make_dirs(dir);
    lib::write_file_atomic(path, out);
    return fresh;
}

std::string libretro_url(const std::string& repo, const std::string& folder, const std::string& name) {
    return "https://raw.githubusercontent.com/libretro-thumbnails/" + repo + "/master/" + folder + "/" + lib::url_encode(name + ".png");
}

}  // namespace

std::string libretro_match(const Item& item, const std::vector<std::string>& names) {
    const std::string wanted = key_of(game_metadata_query(item.name));
    if (wanted.empty()) return "";
    const std::string region = region_label(item.region);
    const char* tag = region == "USA" ? "(USA" : region == "EUR" ? "(Europe" : region == "JAP" ? "(Japan" : region == "ASIA" ? "(Asia" : "";
    std::string best;
    int best_score = -1;
    for (const std::string& name : names) {
        if (key_of(name) != wanted) continue;
        int score = 0;
        if (*tag && name.find(tag) != std::string::npos) score += 4;
        bool special = false;
        for (const char* s : {"(Demo", "(Beta", "(Proto", "(Sample", "(Kiosk", "(Taikenban"})
            if (name.find(s) != std::string::npos) special = true;
        if (!special) score += 2;
        if (std::count(name.begin(), name.end(), '(') <= 1) score += 1;  // the plain release
        if (score > best_score) { best_score = score; best = name; }
    }
    return best;
}

lib::Json open_media(const Item& item) {
    if (const char* repo = libretro_repo(item)) {
        auto shots = lib::Json::array();
        std::string found;
        for (const char* folder : {"Named_Snaps", "Named_Titles"}) {
            const std::string name = libretro_match(item, libretro_index(repo, folder));
            if (name.empty()) continue;
            shots.push(lib::Json(libretro_url(repo, folder, name)));
            found = name;
        }
        if (!shots.items().empty()) {
            auto fields = lib::Json::object();
            fields.set("screenshots", shots);
            fields.set("metadata_source", std::string("https://github.com/libretro-thumbnails/") + repo);
            LOG("open media: %s: libretro-thumbnails \"%s\"", item.title_id.c_str(), found.c_str());
            return fields;
        }
    }
    // Nothing else: an open source's picture for a PS4 title is too often not
    // the game's own (a logo, a photo, another edition), and a wrong picture
    // is worse than none.
    return lib::Json::object();
}

}  // namespace store
