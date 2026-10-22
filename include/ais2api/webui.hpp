// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <optional>
#include <string_view>

namespace ais2api::webui {
struct Asset {
    std::string_view path;
    std::string_view content_type;
    std::string_view body;
    bool immutable;
};
bool enabled();
// Exact embedded assets and known Vue history routes only. Never reads disk.
std::optional<Asset> find(std::string_view path);
} // namespace ais2api::webui
