// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The store is English only; the shared drawing code still calls T(), which
// returns its argument.
#include <string>

namespace ui {

inline const char* T(const char* en) { return en; }
inline std::string T(const std::string& en) { return en; }

}  // namespace ui
