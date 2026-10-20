// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "ais2api/common.hpp"
namespace ais2api {
// Missing platform bundle returns empty; malformed existing bundles fail closed.
fs::path bundled_browser(const fs::path &program_directory);
// Verify all bytes and reject unlisted files before using a packaged runtime.
void verify_browser_bundle(const fs::path &executable);
}
