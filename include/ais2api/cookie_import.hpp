// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "backend.hpp"
namespace ais2api {
struct CookieImportResult {
    std::vector<Account> accounts;
    json::object details;
};
CookieImportResult probe_cookie_import(const json::object &request, unsigned max_accounts,
                                      GenerationBackend &backend, std::stop_token token = {});
}
