// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-paths-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
}
TEST_CASE("default storage belongs to the executable directory", "[config][paths]") {
    CHECK(Config::default_directory() == executable_path().parent_path());
}
TEST_CASE("configuration file paths are independent of the working directory", "[config][paths]") {
    Directory directory;
    const auto file = directory.path / "portable" / "config.json";
    atomic_write(file, R"({"data_dir":"data","browser_executable":"browser/chrome"})");
    auto config = Config::load(file);
    CHECK(config.config_file == fs::absolute(file));
    CHECK(config.data_dir == file.parent_path() / "data");
    CHECK(config.browser_executable == file.parent_path() / "browser/chrome");
    CHECK(string_or(config.to_json(), "data_dir") == path_utf8(config.data_dir));
    auto saved = config.to_json(true);
    CHECK(saved.at("data_dir") == "data");
    CHECK(string_or(saved, "browser_executable") == path_utf8(fs::path("browser") / "chrome"));
    const auto moved = directory.path / "moved" / "config.json";
    atomic_write(moved, json::serialize(saved));
    auto relocated = Config::load(moved);
    CHECK(relocated.data_dir == moved.parent_path() / "data");
    CHECK(relocated.browser_executable == moved.parent_path() / "browser/chrome");
}
TEST_CASE("explicit CLI paths preserve shell semantics and external storage", "[config][paths]") {
    Directory directory;
    const auto file = directory.path / "config.json";
    auto config = Config::load(file, {{"data_dir", "relative-data"},
                                    {"browser_executable", "relative-browser"}});
    CHECK(config.data_dir == fs::absolute("relative-data"));
    CHECK(config.browser_executable == fs::absolute("relative-browser"));
    config.data_dir = directory.path.parent_path() / "external-data";
    config.browser_executable = directory.path.parent_path() / "external-browser";
    CHECK(string_or(config.to_json(true), "data_dir") == path_utf8(config.data_dir));
    CHECK(string_or(config.to_json(true), "browser_executable") == path_utf8(config.browser_executable));
    CHECK_THROWS_AS(Config::load(file, {{"data_dir", ""}}), Error);
}
