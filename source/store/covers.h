// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Cover art. A title's cover_url is drawn through the image cache as
// "art:<url>": the first time it is downloaded, shrunk and kept as a JPEG in
// <data>/Covers, so scrolling back through the store never downloads it again.
#include <string>

#include "store/catalog.h"

namespace store {

void covers_init();
// The image-cache source for a cover URL ("" when there is none).
std::string cover_source(const std::string& cover_url);
// The same for a title. Homebrew is drawn with the app's own icon: icon0.png
// read out of the package at the title's URL (never from what is installed),
// else its cover_url when that is on the web.
std::string cover_source(const Item& item);
// The cached file for a cover URL (for tests).
std::string cover_cache_path(const std::string& cover_url);

}  // namespace store
