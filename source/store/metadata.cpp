#include "store/metadata.h"
#include <algorithm>
#include <atomic>
#include <set>
#include "core/fs.h"
#include "core/text.h"
#include "net/http.h"
#include "platform/platform.h"
#include "store/open_media.h"

namespace store {
namespace {
constexpr const char* kApi = "https://store.playstation.com/store/api/chihiro/00_09_000/";
constexpr const char* kProductPage = "https://store.playstation.com/product/";
constexpr size_t kMaxScreenshots = 12, kMaxTrailers = 6;

struct Storefront { const char* country; const char* language; const char* name; };
constexpr Storefront kUS{"US", "en", "USA"}, kGB{"GB", "en", "Europe"}, kJP{"JP", "ja", "Japan"}, kHK{"HK", "en", "Asia"};

// The stores to look in: the item's region first, then the others (a
// content list's region can be wrong or missing, and not every title is
// sold everywhere).
std::vector<Storefront> storefronts(const Item& item) {
    const std::string region = region_label(item.region);
    std::vector<Storefront> out;
    if (region == "EUR") out.push_back(kGB);
    else if (region == "JAP") out.push_back(kJP);
    else if (region == "ASIA") out.push_back(kHK);
    else out.push_back(kUS);
    for (const Storefront& s : {kUS, kGB, kJP, kHK})
        if (s.country != out[0].country) out.push_back(s);
    return out;
}

// PS1, PS2 and PSP titles: the store may only sell them for another console;
// their pictures are still the game's own.
bool classic(const Item& item) {
    return item.category == Category::PS1 || item.category == Category::PS2 || item.category == Category::PSP;
}

// The store a content ID belongs to: its first letters name the region.
Storefront storefront_of(const std::string& content_id) {
    if (lib::starts_with(content_id, "EP")) return kGB;
    if (lib::starts_with(content_id, "JP")) return kJP;
    if (lib::starts_with(content_id, "HP")) return kHK;
    return kUS;
}

std::string path_of(const Storefront& s) { return std::string(s.country) + "/" + s.language + "/999/"; }

// "UP2047-CUSA01014_00-DI2MAINSIEAPS400"
bool content_id_valid(const std::string& id) {
    if (id.size() != 36 || id[6] != '-' || id[16] != '_' || id[19] != '-') return false;
    for (char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}
bool title_id_valid(const std::string& id) {
    if (id.size() != 9) return false;
    for (int i = 0; i < 9; ++i) {
        const char c = id[i];
        if (i < 4 ? !(c >= 'A' && c <= 'Z') : !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

// The store refuses a client that asks too often (403 or 429): automatic
// lookups then stop for a while rather than ask again on every page.
std::atomic<uint64_t> g_refused_until{0};
constexpr uint64_t kRefusedPauseMs = 10 * 60 * 1000;
const char* const kNoLongerListed = "That game is no longer listed on the PlayStation Store.";
const char* const kRefused = "The PlayStation Store is refusing requests for now. Try again in a few minutes.";

lib::Json get_json(const std::string& path, std::string& problem, bool* missing = nullptr) {
    if (platform::now_ms() < g_refused_until) { problem = kRefused; return {}; }
    lib::HttpRequest request;
    request.url = kApi + path;
    request.timeout_seconds = 15;
    request.max_bytes = 4 * 1024 * 1024;
    const auto response = lib::http_request(request);
    if (missing) *missing = response.status == 404;
    if (response.status == 403 || response.status == 429) {
        g_refused_until = platform::now_ms() + kRefusedPauseMs;
        problem = kRefused;
        return {};
    }
    if (!response.ok()) {
        if (response.status != 404) problem = "PlayStation Store: " + lib::describe_failure(response);
        return {};
    }
    bool ok = false;
    const auto root = lib::Json::parse(response.body, &ok);
    if (!ok || !root.is_object()) problem = "PlayStation Store: an answer FPKGi+ could not read. Try again later.";
    return root;
}

std::string normalized(const std::string& text) {
    std::string out;
    for (unsigned char c : lib::lower(game_metadata_query(text))) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 128) out += (char)c;
        else out += ' ';
    }
    // The store writes trademarks into names ("The Last Of Us™"): drop what
    // is left of them as well as punctuation.
    out = lib::replace_all(out, "\xE2\x84\xA2", " ");
    out = lib::replace_all(out, "\xC2\xAE", " ");
    return lib::collapse_space(out);
}

bool is_game_listing(const lib::Json& link) {
    const std::string category = link["top_category"].str();
    return category == "downloadable_game" || category == "demo" || category == "application";
}

// What can stand for the item: the game or app itself; for an add-on or a
// theme its own listing too (else the game it belongs to).
bool is_listing_for(const lib::Json& link, const Item& item) {
    if (is_game_listing(link)) return true;
    const std::string category = link["top_category"].str();
    if (item.category == Category::DLC) return category == "add_on";
    if (item.category == Category::Themes) return category == "theme";
    return false;
}

bool plays_on(const lib::Json& link, const Item& item) {
    if (classic(item)) return true;
    const bool ps5 = item.ps5 || item.category == Category::PS5;
    for (const auto& platform : link["playable_platform"].items())
        if (platform.str().find(ps5 ? "PS5" : "PS4") != std::string::npos) return true;
    return false;
}

// How a store name fits the title's: 2 the same; 1 the same game with only
// release words added on either side ("The Last of Us" and "The Last of Us
// Remastered", "Until Dawn" and "Until Dawn 2015"); 0 not the same game
// ("... Part II", "... 2" add a word that is not a release word).
int name_match(const std::string& store_name, const std::string& name) {
    const std::string a = normalized(store_name), b = normalized(name);
    if (a.empty() || b.empty()) return 0;
    if (a == b) return 2;
    const std::string& longer = a.size() > b.size() ? a : b;
    const std::string& shorter = a.size() > b.size() ? b : a;
    if (longer.compare(0, shorter.size(), shorter) != 0 || longer[shorter.size()] != ' ') return 0;
    static const std::set<std::string> release_words = {"remastered", "remaster", "edition", "standard", "digital", "complete",
        "definitive", "deluxe", "ps4", "ps5", "game", "of", "the", "year", "goty", "full", "hd"};
    for (const std::string& word : lib::split(longer.substr(shorter.size() + 1), ' ')) {
        const bool year = word.size() == 4 && word.find_first_not_of("0123456789") == std::string::npos;
        if (!word.empty() && !year && !release_words.count(word)) return 0;
    }
    return 1;
}

// A PS4 game sold as one PS4 and PS5 product is often listed for PS5 only:
// the same game, so its pictures stand in when the store has no PS4 listing.
bool cross_gen(const lib::Json& link, const Item& item) {
    if (item.ps5 || item.category == Category::PS5 || classic(item)) return false;
    for (const auto& platform : link["playable_platform"].items())
        if (platform.str().find("PS5") != std::string::npos) return true;
    return false;
}

// How well a store listing fits the item (higher is better; < 0: not it).
int listing_score(const lib::Json& link, const Item& item) {
    const std::string id = link["id"].str();
    if (!content_id_valid(id) || !is_listing_for(link, item)) return -1;
    // An add-on or theme is only taken by its own name: a title ID holds
    // many (a game's songs, avatars), and their pictures are not this one's.
    if (!is_game_listing(link) && name_match(link["name"].str(), item.name) == 0) return -1;
    int score = 0;
    if (id.compare(7, 9, item.title_id) == 0) score += 8;
    if (link["top_category"].str() == "downloadable_game") score += 2;
    const std::string type = link["game_contentType"].str();
    if (type == "Full Game" || type == "PSN Game" || type == "Game") score += 1;
    if (normalized(link["name"].str()) == normalized(item.name)) score += 2;
    if (plays_on(link, item)) score += 1;
    return score;
}

// The game itself among everything sold under its title ID (add-ons too:
// some games have hundreds, so a few pages are read).
std::string find_by_title_id(const Item& item, const Storefront& store, std::string& problem) {
    std::string best;
    int best_score = -1;
    for (int start = 0; start < 500; start += 100) {
        const auto page = get_json("container/" + path_of(store) + item.title_id + "_00?size=100&start=" + std::to_string(start), problem);
        if (!problem.empty()) return "";
        for (const auto& link : page["links"].items()) {
            const std::string id = link["id"].str();
            if (!content_id_valid(id) || id.compare(7, 9, item.title_id) != 0) continue;
            const int score = listing_score(link, item);
            if (score > best_score) { best_score = score; best = id; }
        }
        if (!best.empty() || start + 100 >= page["total_results"].integer()) break;
    }
    return best;
}

lib::Json search(const Storefront& store, const std::string& query, std::string& problem) {
    return get_json("tumbler/" + path_of(store) + lib::url_encode(query) + "?suggested_size=20&mode=game", problem);
}

// Sony's picture and video servers (*.dl.playstation.net) answer plain HTTP
// as well: used that way, the console's TLS (which refuses some of their
// certificate chains) is not involved in showing public pictures and trailers.
std::string media_url(const std::string& url) {
    if (!lib::starts_with(url, "https://")) return url;
    const size_t end = url.find('/', 8);
    const std::string host = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
    const std::string suffix = ".dl.playstation.net";
    if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
        return "http://" + url.substr(8);
    return url;
}

std::string join_values(const lib::Json& values, const char* separator = ", ") {
    std::string out;
    for (const auto& v : values.items()) {
        const std::string text = lib::trim(v.str());
        if (text.empty()) continue;
        if (!out.empty()) out += separator;
        out += text;
    }
    return out;
}
}  // namespace

std::string game_metadata_query(const std::string& name) {
    std::string query = lib::decode_entities(name);
    // Catalogs commonly append region/language/version tags. Keep subtitles
    // and editions intact so an automatic match cannot silently change games.
    const size_t tags = query.find('[');
    if (tags != std::string::npos && tags > 0) query.resize(tags);
    query = lib::replace_all(query, "\xE2\x84\xA2", ""); // trademark
    query = lib::replace_all(query, "\xC2\xAE", "");
    return lib::collapse_space(query);
}

bool needs_game_metadata(const Item& item) {
    // Everything but homebrew (and its emulators): those are not on the
    // store, and keep their project's own description.
    const bool sold = item.category != Category::Homebrew && item.category != Category::Emulators;
    // What is found is kept by title ID: a title without one is not looked up.
    return sold && !item.title_id.empty() && !lib::starts_with(item.metadata_source, kProductPage);
}

namespace {
MetadataResult auto_lookup(const Item& item) {
    MetadataResult result;
    const auto stores = storefronts(item);
    // By title ID, in every store: exact, whatever the content list calls the game.
    if (title_id_valid(item.title_id)) for (const Storefront& store : stores) {
        const std::string id = find_by_title_id(item, store, result.problem);
        if (!result.problem.empty()) return result;
        if (id.empty()) continue;
        MetadataResult found = fetch_game_metadata(item, id);
        // Listed under its title ID but withdrawn from sale: try by name.
        if (found.problem != kNoLongerListed) return found;
    }
    // By name, in the region's store and the next: a listing with this title
    // ID, else one with exactly this name for this console.
    const std::string query = game_metadata_query(item.name);
    for (size_t s = 0; s < stores.size() && s < 2 && !query.empty(); ++s) {
        const Storefront& store = stores[s];
        const auto found = search(store, query, result.problem);
        if (!result.problem.empty()) return result;
        // The best listing by name: this console's over a PS5 cross-gen one,
        // the same name over one with release words added, the game itself
        // over a demo.
        std::string by_name;
        int best = 0;
        for (const auto& link : found["links"].items()) {
            const std::string id = link["id"].str();
            if (!content_id_valid(id) || !is_listing_for(link, item)) continue;
            if (id.compare(7, 9, item.title_id) == 0 && is_game_listing(link)) return fetch_game_metadata(item, id);
            const int match = name_match(link["name"].str(), query);
            if (match == 0) continue;
            const bool here = plays_on(link, item);
            if (!here && !cross_gen(link, item)) continue;
            const int score = match * 10 + (here ? 5 : 0) + (link["top_category"].str() == "downloadable_game" ? 2 : 0);
            if (score > best) { best = score; by_name = id; }
        }
        if (!by_name.empty()) return fetch_game_metadata(item, by_name);
    }
    result.problem = "Not found on the PlayStation Store. Use Options > Find game info to search by name.";
    return result;
}
}  // namespace

MetadataResult auto_game_metadata(const Item& item) {
    if (!needs_game_metadata(item)) return {};
    MetadataResult result = auto_lookup(item);
    result.retry_later = result.problem == kRefused || lib::starts_with(result.problem, "PlayStation Store:");
    if (!result.problem.empty()) {
        // Not on the store (or not reachable): screenshots from open sources.
        // The store is asked again next time; what it has then replaces these.
        lib::Json open = open_media(item);
        if (!open["screenshots"].items().empty()) {
            result.fields = open;
            result.problem.clear();
        }
    }
    return result;
}

MetadataSearch search_game_metadata(const Item& item, const std::string& name) {
    MetadataSearch result;
    std::set<std::string> seen;
    for (const Storefront& store : storefronts(item)) {
        const auto found = search(store, name, result.problem);
        if (!result.problem.empty()) return result;
        for (const auto& link : found["links"].items()) {
            const std::string id = link["id"].str();
            if (!content_id_valid(id) || !is_game_listing(link) || !seen.insert(id).second) continue;
            std::string about = join_values(link["playable_platform"], "/");
            const std::string type = link["game_contentType"].str();
            if (!type.empty()) about += (about.empty() ? "" : ", ") + type;
            about += (about.empty() ? "" : ", ") + id.substr(7, 9) + " (" + store.name + ")";
            result.matches.push_back({id, lib::trim(link["name"].str()), about});
        }
    }
    if (result.matches.empty()) result.problem = "No PlayStation Store matches. Try another game name.";
    return result;
}

std::string store_description_text(const std::string& html) {
    std::string s = html;
    // The store opens many PS4 descriptions with a bold notice about playing on PS5.
    for (size_t at = 0; (at = s.find("<b>", at)) != std::string::npos;) {
        const size_t end = s.find("</b>", at);
        if (end == std::string::npos) break;
        const std::string inside = s.substr(at, end - at);
        if (inside.find("PS5") != std::string::npos &&
            (inside.find("system software") != std::string::npos || inside.find("PlayStation.com/bc") != std::string::npos)) {
            s.erase(at, end + 4 - at);
            while (lib::starts_with(s.substr(at), "<br>") || lib::starts_with(s.substr(at), "<br/>") ||
                   lib::starts_with(s.substr(at), "<br />"))
                s.erase(at, s.find('>', at) + 1 - at);
        } else {
            at = end;
        }
    }
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '<') {
            const size_t end = s.find('>', i);
            if (end == std::string::npos) break;
            const std::string tag = lib::lower(s.substr(i + 1, end - i - 1));
            if (lib::starts_with(tag, "br") || tag == "/p" || tag == "/li" || tag == "/div") out += '\n';
            else if (tag == "li" || lib::starts_with(tag, "li ")) out += "\xE2\x80\xA2 ";
            i = end + 1;
            continue;
        }
        out += s[i++];
    }
    out = lib::decode_entities(out);
    // Lines trimmed; at most one blank line between paragraphs.
    std::string text, line;
    int blank = 0;
    const auto flush = [&] {
        const std::string trimmed = lib::trim(line);
        line.clear();
        if (trimmed.empty()) { ++blank; return; }
        if (!text.empty()) text += blank > 0 ? "\n\n" : "\n";
        text += trimmed;
        blank = 0;
    };
    for (char c : out) {
        if (c == '\n') flush();
        else if (c != '\r') line += c;
    }
    flush();
    return text;
}

MetadataResult parse_game_metadata(const std::string& content_id, const lib::Json& product) {
    MetadataResult result;
    if (!content_id_valid(content_id) || !product.is_object() || product["id"].str() != content_id) {
        result.problem = "That PlayStation Store entry could not be read.";
        return result;
    }
    auto fields = lib::Json::object();
    fields.set("metadata_source", kProductPage + content_id);
    const std::string description = store_description_text(product["long_desc"].str());
    if (!description.empty()) fields.set("description", description);
    const std::string publisher = lib::trim(product["provider_name"].str());
    if (!publisher.empty()) fields.set("publisher", publisher);
    const std::string date = product["release_date"].str();
    if (date.size() >= 10 && !game_date(date.substr(0, 10)).empty()) {
        fields.set("release_date", date.substr(0, 10));
        fields.set("release_date_scope", std::string("PlayStation Store (") + storefront_of(content_id).name + ")");
    }
    const auto& meta = product["metadata"];
    std::string genres = join_values(meta["genre"]["values"]);
    if (genres.empty()) genres = join_values(meta["game_genre"]["values"]);
    if (!genres.empty()) fields.set("genres", genres);
    const std::string players = join_values(meta["cn_numberOfPlayers"]["values"]);
    if (!players.empty()) fields.set("players", players + (players == "1" ? " player" : " players"));
    const std::string rating = lib::trim(product["content_rating"]["description"].str());
    if (!rating.empty()) fields.set("rating", rating);

    const auto web = [](const std::string& url) { return lib::starts_with(url, "https://") || lib::starts_with(url, "http://"); };
    auto screenshots = lib::Json::array();
    std::set<std::string> seen;
    for (const auto& shot : product["mediaList"]["screenshots"].items()) {
        const std::string url = shot["url"].str();
        if (web(url) && seen.insert(url).second && screenshots.items().size() < kMaxScreenshots) screenshots.push(lib::Json(media_url(url)));
    }
    auto trailers = lib::Json::array();
    // Trailers are streamed: the console's player plays HLS over the network
    // (a web MP4 it only opens as a local file), so only trailers the store
    // gives an HLS stream for are kept.
    for (const auto& preview : product["mediaList"]["previews"].items()) {
        std::string url = preview["streamUrl"].str();
        if (!web(url) && lib::lower(preview["url"].str()).find(".m3u8") != std::string::npos) url = preview["url"].str();
        if (!web(url) || !seen.insert(url).second || trailers.items().size() >= kMaxTrailers) continue;
        auto trailer = lib::Json::object();
        trailer.set("url", media_url(url));
        const auto& shots = preview["shots"].items();
        if (!shots.empty() && web(shots[0].str())) trailer.set("poster", media_url(shots[0].str()));
        trailers.push(trailer);
    }
    fields.set("screenshots", screenshots);
    fields.set("trailers", trailers);
    fields.set("format", lib::Json(3.0));  // trailers as HLS streams: see Catalog::load
    result.fields = fields;
    return result;
}

MetadataResult fetch_game_metadata(const Item& item, const std::string& content_id) {
    (void)item;
    MetadataResult result;
    if (!content_id_valid(content_id)) { result.problem = "Invalid PlayStation Store content ID."; return result; }
    bool missing = false;
    const auto product = get_json("container/" + path_of(storefront_of(content_id)) + content_id, result.problem, &missing);
    if (!result.problem.empty()) return result;
    if (missing) { result.problem = kNoLongerListed; return result; }
    return parse_game_metadata(content_id, product);
}

bool save_game_metadata(const Item& item, const lib::Json& fields) {
    const std::string path = lib::join_path(platform::data_root(), "store-metadata.json");
    std::string text;
    lib::read_file(path, text);
    bool ok = false;
    auto root = lib::Json::parse(text, &ok);
    if (!ok || !root.is_object()) root = lib::Json::object();
    root.set(item.title_id, fields);
    return lib::write_file_atomic(path, root.dump(true));
}

void apply_game_metadata(Item& item, const lib::Json& fields) {
    apply_metadata(item, fields);
    std::string text;
    if (lib::read_file(lib::join_path(platform::data_root(), "metadata.json"), text, 8 << 20)) {
        bool ok = false;
        const auto local = lib::Json::parse(text, &ok);
        if (ok && local.is_object()) apply_metadata(item, local[item.title_id]);
    }
}
}
