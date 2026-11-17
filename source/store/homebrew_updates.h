#pragma once
#include "store/catalog.h"
#include "store/installer.h"

namespace store {
std::string sfo_string(const std::string& bytes, const std::string& key);
// -1/0/1: older/equal/newer; 2: not a comparable numeric app version.
int compare_app_versions(const std::string& left, const std::string& right);
struct HomebrewUpdate {
    Item item;
    std::string installed_version, latest_version, problem;
    bool available = false, downloaded = false;
    std::string downloaded_path;
};
HomebrewUpdate inspect_homebrew_update(const Item& item, const lib::Json& release,
    const std::string& installed_sfo, const PackageReader& latest_package);
HomebrewUpdate check_homebrew_update(const Item& item, const std::string& downloaded_path = "");
}
