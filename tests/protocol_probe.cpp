// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Developer-only, no-network probe. Input must be synthetic request fixtures.
// Never installed with the application; no database or browser is instantiated.
#include "ais2api/protocol.hpp"
#include <iostream>

int main() {
    using namespace ais2api;
    try {
        std::string input;
        char c;
        while (std::cin.get(c)) {
            if (input.size() >= 8 * 1024 * 1024)
                throw Error(413, "fixture_limit", "Fixture input too large");
            input += c;
        }
        const auto requests = json::parse(input).as_array();
        if (requests.size() > 128)
            throw Error(413, "fixture_limit", "Too many fixtures");
        Config config;
        json::array results;
        for (const auto& request : requests) {
            try {
                auto normalized = normalize_openai(request.as_object(), config);
                results.push_back(json::object{{"accepted", true}, {"request", normalized.gemini}});
            } catch (const Error& error) {
                results.push_back(json::object{{"accepted", false}, {"status", error.status}});
            }
        }
        std::cout << json::serialize(results) << '\n';
        return 0;
    } catch (...) {
        std::cerr << "Offline fixture probe failed\n";
        return 1;
    }
}
