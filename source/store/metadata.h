#pragma once
#include <string>
#include <vector>
#include "core/json.h"
#include "store/catalog.h"

namespace store {
struct MetadataMatch { std::string id, name, description; };
struct MetadataSearch { std::vector<MetadataMatch> matches; std::string problem; };
struct MetadataResult { lib::Json fields; std::string problem; };
MetadataSearch search_game_metadata(const std::string& name);
std::string game_metadata_query(const std::string& name);
bool needs_game_metadata(const Item& item);
MetadataResult auto_game_metadata(const Item& item);
MetadataResult fetch_game_metadata(const Item& item, const std::string& id, bool require_platform = false);
MetadataResult parse_game_metadata(const Item& item, const std::string& id,
                                   const lib::Json& entity, const lib::Json& labels);
bool save_game_metadata(const Item& item, const lib::Json& fields);
// Provider fields remain subordinate to the user's local metadata overrides.
void apply_game_metadata(Item& item, const lib::Json& fields);
}
