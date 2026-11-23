// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The background downloader exists only on the console: the host build
// downloads in the app.
#include "store/service.h"

namespace store {

bool service_start() { return false; }
std::string service_status() { return "Console only"; }
std::string service_restart() { return "The background downloader runs only on the console."; }
std::string service_remove() { return "The background downloader runs only on the console."; }

}  // namespace store
