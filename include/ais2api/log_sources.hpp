// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "common.hpp"
namespace ais2api {
// Only explicitly enumerated application logs are readable. Never accepts a path.
json::object log_sources(const fs::path& directory);
json::object read_log_source(const fs::path& directory, std::string_view source,
                             std::string_view cursor, std::size_t bytes = 49152);
}
