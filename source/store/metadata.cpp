#include "store/metadata.h"
#include <algorithm>
#include <set>
#include "core/fs.h"
#include "core/text.h"
#include "net/http.h"
#include "platform/platform.h"

namespace store {
namespace {
bool entity_id(const std::string& s) {
    if (s.size() < 2 || s[0] != 'Q') return false;
    return s.find_first_not_of("0123456789", 1) == std::string::npos;
}
lib::Json get_json(const std::string& query, std::string& problem) {
    lib::HttpRequest request;
    request.url = "https://www.wikidata.org/w/api.php?format=json&maxlag=5&" + query;
    request.headers.emplace_back("User-Agent", "FPKGiPlus/1.2 (https://github.com/jw-notes)");
    request.timeout_seconds = 15;
    request.max_bytes = 2 * 1024 * 1024;
    const auto response = lib::http_request(request);
    if (!response.ok()) { problem = "Wikidata: " + lib::describe_failure(response); return {}; }
    bool ok = false;
    const auto root = lib::Json::parse(response.body, &ok);
    if (!ok || root.has("error")) problem = "Wikidata is unavailable or busy. Try again later.";
    return root;
}
const lib::Json& value(const lib::Json& statement) { return statement["mainsnak"]["datavalue"]["value"]; }
std::string label(const lib::Json& labels, const std::string& id) {
    return labels[id]["labels"]["en"]["value"].str();
}
std::string named_values(const lib::Json& claims, const char* property, const lib::Json& labels) {
    std::string out;
    std::set<std::string> seen;
    for (const auto& statement : claims[property].items()) {
        if (statement["rank"].str() == "deprecated") continue;
        const std::string name = label(labels, value(statement)["id"].str());
        if (name.empty() || !seen.insert(name).second) continue;
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}
std::string platform_id(const Item& item) {
    if (item.ps5 || item.category == Category::PS5) return "Q63184502";
    if (item.category == Category::PS1) return "Q10677";
    if (item.category == Category::PS2) return "Q10680";
    if (item.category == Category::PSP) return "Q170325";
    return "Q5014725"; // PlayStation 4
}
std::string platform_name(const Item& item) {
    if (item.ps5 || item.category == Category::PS5) return "PS5";
    if (item.category == Category::PS1) return "PS1";
    if (item.category == Category::PS2) return "PS2";
    if (item.category == Category::PSP) return "PSP";
    return "PS4";
}
}

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
    const bool game = item.category == Category::Games || item.category == Category::PS1 ||
        item.category == Category::PS2 || item.category == Category::PSP ||
        item.category == Category::PS5 || item.category == Category::Demos;
    return game && !item.title_id.empty() &&
        !lib::starts_with(item.metadata_source, "https://www.wikidata.org/wiki/Q");
}

MetadataResult auto_game_metadata(const Item& item) {
    MetadataResult result;
    const std::string query = game_metadata_query(item.name);
    if (!needs_game_metadata(item) || query.empty()) return result;
    const auto search = search_game_metadata(query);
    if (!search.problem.empty()) { result.problem = search.problem; return result; }
    auto normalized = [](const std::string& text) {
        std::string out;
        for (unsigned char c : lib::lower(game_metadata_query(text))) {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 128) out += (char)c;
            else out += ' ';
        }
        return lib::collapse_space(out);
    };
    const std::string wanted = normalized(query);
    std::set<std::string> exact;
    for (const auto& match : search.matches)
        if (normalized(match.name) == wanted) exact.insert(match.id);
    if (exact.size() != 1) {
        result.problem = "No unambiguous game match. Use Options > Find game info to choose one.";
        return result;
    }
    return fetch_game_metadata(item, *exact.begin(), true);
}

MetadataSearch search_game_metadata(const std::string& name) {
    MetadataSearch result;
    const auto root = get_json("action=wbsearchentities&language=en&uselang=en&type=item&limit=8&search=" +
                              lib::url_encode(name), result.problem);
    if (!result.problem.empty()) return result;
    for (const auto& match : root["search"].items())
        if (entity_id(match["id"].str())) result.matches.push_back({match["id"].str(), match["label"].str(), match["description"].str()});
    if (result.matches.empty()) result.problem = "No Wikidata matches. Try another game name.";
    return result;
}

MetadataResult parse_game_metadata(const Item& item, const std::string& id,
                                   const lib::Json& entity, const lib::Json& labels) {
    MetadataResult result;
    const auto& claims = entity["claims"];
    bool is_game = !claims["P400"].items().empty();
    for (const auto& s : claims["P31"].items()) if (value(s)["id"].str() == "Q7889") is_game = true;
    if (!entity_id(id) || !is_game) { result.problem = "That Wikidata entry is not identified as a game or platform release."; return result; }
    auto fields = lib::Json::object();
    fields.set("metadata_source", "https://www.wikidata.org/wiki/" + id);
    const std::string description = entity["descriptions"]["en"]["value"].str();
    if (!description.empty()) fields.set("description", description);
    for (const auto& pair : {std::make_pair("developer", "P178"), {"publisher", "P123"}, {"genres", "P136"}, {"languages", "P407"}}) {
        const std::string text = named_values(claims, pair.second, labels);
        if (!text.empty()) fields.set(pair.first, text);
    }
    // Select only matching platform/region claims when possible. Never turn a
    // year-precision claim into a fabricated January 1st release date.
    std::string date, scope;
    int best = -1;
    const std::string wanted = platform_id(item);
    const std::string region = item.region == "EUR" ? "Q46" : item.region == "USA" ? "Q30" :
                               item.region == "JAP" ? "Q17" : item.region == "ASIA" ? "Q48" : "";
    for (const auto& statement : claims["P577"].items()) {
        if (statement["rank"].str() == "deprecated") continue;
        const auto& v = value(statement);
        const std::string time = v["time"].str();
        const int precision = (int)v["precision"].integer();
        if (time.size() < 11 || time[0] != '+' || precision < 9 ||
            v["calendarmodel"].str() != "http://www.wikidata.org/entity/Q1985727") continue;
        const std::string candidate = game_date(time.substr(1, precision == 9 ? 4 : precision == 10 ? 7 : 10));
        if (candidate.empty()) continue;
        const auto& qualifiers = statement["qualifiers"];
        bool matches_platform = false, matches_region = false;
        std::string territory;
        for (const auto& q : qualifiers["P400"].items())
            if (q["datavalue"]["value"]["id"].str() == wanted) matches_platform = true;
        for (const auto& q : qualifiers["P291"].items()) {
            const std::string qid = q["datavalue"]["value"]["id"].str();
            if (!region.empty() && qid == region) matches_region = true;
            const std::string text = label(labels, qid);
            if (!text.empty()) { if (!territory.empty()) territory += ", "; territory += text; }
        }
        const int score = matches_platform ? (matches_region ? 3 : qualifiers["P291"].items().empty() ? 2 : 1) : 0;
        if (score < best || (score == best && !date.empty() && candidate >= date)) continue;
        best = score; date = candidate;
        scope = matches_platform ? platform_name(item) + (territory.empty() ? " (region unspecified)" : " (" + territory + ")") :
                                   "Earliest listed release (any platform/region)";
    }
    if (!date.empty()) { fields.set("release_date", date); fields.set("release_date_scope", scope); }
    result.fields = fields;
    return result;
}

MetadataResult fetch_game_metadata(const Item& item, const std::string& id, bool require_platform) {
    MetadataResult result;
    if (!entity_id(id)) { result.problem = "Invalid Wikidata identifier."; return result; }
    const auto root = get_json("action=wbgetentities&ids=" + id + "&props=claims%7Clabels%7Cdescriptions&languages=en", result.problem);
    if (!result.problem.empty()) return result;
    const auto& entity = root["entities"][id];
    if (require_platform) {
        bool supported = false;
        for (const auto& statement : entity["claims"]["P400"].items())
            if (statement["rank"].str() != "deprecated" && value(statement)["id"].str() == platform_id(item)) supported = true;
        if (!supported) {
            result.problem = "The matching entry does not confirm this console platform. Use Options > Find game info.";
            return result;
        }
    }
    std::set<std::string> ids;
    for (const char* property : {"P178", "P123", "P136", "P407", "P577"}) {
        for (const auto& s : entity["claims"][property].items()) {
            const std::string target = value(s)["id"].str();
            if (entity_id(target)) ids.insert(target);
            for (const auto& q : s["qualifiers"]["P291"].items()) {
                const std::string target = q["datavalue"]["value"]["id"].str();
                if (entity_id(target)) ids.insert(target);
            }
        }
    }
    std::string joined;
    for (const auto& qid : ids) {
        if (joined.size() > 450) break;
        if (!joined.empty()) joined += "%7C";
        joined += qid;
    }
    lib::Json labels = lib::Json::object();
    if (!joined.empty()) {
        const auto named = get_json("action=wbgetentities&ids=" + joined + "&props=labels&languages=en", result.problem);
        if (!result.problem.empty()) return result;
        labels = named["entities"];
    }
    return parse_game_metadata(item, id, entity, labels);
}

bool save_game_metadata(const Item& item, const lib::Json& fields) {
    const std::string path = lib::join_path(platform::data_root(), "wikidata-metadata.json");
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
