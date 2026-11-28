#pragma once
// Game information from the PlayStation Store: overview, publisher, release
// date, genre, players, age rating, screenshots and trailers. A title is found
// by its title ID in the store of its region, else by name; what is found is
// kept in <data>/store-metadata.json so the store is asked once per title.
#include <string>
#include <vector>
#include "core/json.h"
#include "store/catalog.h"

namespace store {
struct MetadataMatch { std::string id, name, description; };
struct MetadataSearch { std::vector<MetadataMatch> matches; std::string problem; };
struct MetadataResult {
    lib::Json fields;
    std::string problem;
    // Failed for now (no network, the store refusing requests): worth asking again later.
    bool retry_later = false;
};
// Games in the store of the item's region whose name matches (id: the content ID).
MetadataSearch search_game_metadata(const Item& item, const std::string& name);
std::string game_metadata_query(const std::string& name);
bool needs_game_metadata(const Item& item);
MetadataResult auto_game_metadata(const Item& item);
MetadataResult fetch_game_metadata(const Item& item, const std::string& content_id);
// The fields of a store product (chihiro "container" JSON).
MetadataResult parse_game_metadata(const std::string& content_id, const lib::Json& product);
// A store description (HTML) as plain text, without the PS5 compatibility notice.
std::string store_description_text(const std::string& html);
bool save_game_metadata(const Item& item, const lib::Json& fields);
// Provider fields remain subordinate to the user's local metadata overrides.
void apply_game_metadata(Item& item, const lib::Json& fields);
}
