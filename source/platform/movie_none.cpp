// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// No video: the host build (the trailer page shows its picture instead) and
// the background downloader, which never plays one and so does not carry the
// player's libraries (platform/movie_mpv_ps4.cpp).
#include "platform/platform.h"

namespace platform {

bool movie_open(const std::string&, std::string& problem) {
    problem = "Trailers play on the console.";
    return false;
}
bool movie_draw(uint32_t*, int, int) { return false; }
bool movie_active() { return false; }
uint64_t movie_position_ms() { return 0; }
uint64_t movie_duration_ms() { return 0; }
void movie_pause(bool) {}
void movie_seek(uint64_t) {}
std::string movie_problem() { return ""; }
void movie_close() {}

}  // namespace platform
