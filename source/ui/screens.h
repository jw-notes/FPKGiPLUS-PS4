// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <memory>

#include "store/catalog.h"
#include "ui/screen.h"

namespace ui {

// Starts loading the catalogue in the background; the store tabs appear when
// it is ready.
void reload_catalog();

std::unique_ptr<Screen> make_details_screen(const store::Item& item);
// Download now, or let the console download it, as Settings say.
void get_item(const store::Item& item);
// The store's options: search, sort, regions, how to download. `installed`:
// the Installed tab's, with Show (which list) in place of the regions.
void open_options_menu(store::Category category = store::Category::Games, bool installed = false);

}  // namespace ui
