// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The background downloader (FPKP00002, source/service/main.cpp): FPKGi+'s
// download queue in a console background service, so downloads go on after
// FPKGi+ is closed and while other apps and games run. In rest mode it is
// suspended, and its downloads continue when the console wakes.
//
// It is installed the way Itemzflow installs its daemon: FPKGi+ makes the
// system partition writable for a moment and copies the service (from its own
// package) to /system/vsh/app/FPKP00002, with the param.sfo of a system
// background service (NPXS21007). It is replaced when FPKGi+ carries a
// different build of it. Needs FPKGi+ to be out of the sandbox (GoldHEN).
#include <string>

namespace store {

// At start, before the download queue is restored: installs or updates the
// service, starts it if it is not running and waits for it to answer. True
// when it runs the queue (then downloads().use_service(true) is set).
bool service_start();
// "Running", "Not running", "Not installed", "Unavailable (...)".
std::string service_status();
// Stops and starts it again (a message for a toast).
std::string service_restart();
// Stops it and deletes it from the system partition (a message for a toast).
// Downloads go back to running in FPKGi+ the next time it is opened.
std::string service_remove();

}  // namespace store
