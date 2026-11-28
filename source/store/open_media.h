#pragma once
// Screenshots from an open source, for titles the PlayStation Store has none
// for (store/metadata.h): PS1, PS2 and PSP games from libretro-thumbnails
// (in-game screenshots and title screens, matched by name and region). Only
// the game's own pictures: nothing for other titles rather than a wrong one.
// Open sources hold no trailers the console can play.
#include <string>
#include <vector>

#include "core/json.h"
#include "store/catalog.h"

namespace store {

// Fields as store/metadata.h keeps them ("screenshots", "metadata_source"),
// or an empty object when nothing was found.
lib::Json open_media(const Item& item);

// The libretro-thumbnails name that best fits a title among `names` (file
// names without ".png"); "" when none does. For tests.
std::string libretro_match(const Item& item, const std::vector<std::string>& names);

}  // namespace store
