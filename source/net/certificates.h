// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace lib {
// Native HTTPS accepts individual complete PEM records with CRLF endings.
std::vector<std::string> split_ca_certificates(const std::string& bundle);
}
