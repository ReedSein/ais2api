// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "common.hpp"
namespace ais2api {
struct Limits {
    std::size_t request_bytes = 32 * 1024 * 1024;
    std::size_t object_bytes = 64 * 1024 * 1024;
    std::size_t relay_bytes = 1024 * 1024;
    std::size_t queue_size = 8;
    unsigned queue_seconds = 30;
    unsigned generation_seconds = 240;
    unsigned stream_idle_seconds = 240;
    unsigned max_attempts = 3;
};
struct Config {
    int version = 1;
    std::string bind = "127.0.0.1";
    unsigned short port = 11451;
    fs::path data_dir;
    fs::path config_file;
    fs::path browser_executable;
    std::string proxy;
    bool headless = true;
    Limits limits;
    json::object model_aliases;
    json::array configured_models;
    json::object model_defaults;
    static fs::path default_directory();
    static Config load(const fs::path& file, const json::object& overrides = {});
    json::object to_json() const;
    void validate() const;
};
}
