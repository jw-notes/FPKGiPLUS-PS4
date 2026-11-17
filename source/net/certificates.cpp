// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
#include "net/certificates.h"

namespace lib {
std::vector<std::string> split_ca_certificates(const std::string& bundle) {
    const std::string begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    std::vector<std::string> certificates;
    size_t at = 0;
    while ((at = bundle.find(begin, at)) != std::string::npos) {
        const size_t finish = bundle.find(end, at + begin.size());
        if (finish == std::string::npos || bundle.find(begin, at + begin.size()) < finish) return {};
        std::string pem;
        for (size_t i = at; i < finish + end.size(); ++i) {
            const char c = bundle[i];
            if (c == '\r') continue;
            if (c == '\n') pem += '\r';
            pem += c;
        }
        pem += "\r\n";
        certificates.push_back(std::move(pem));
        at = finish + end.size();
    }
    return certificates;
}
}
