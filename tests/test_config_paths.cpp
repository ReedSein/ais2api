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
    CHECK(Config{}.port == 18451);
    CHECK(Config{}.api_key.empty());
}
TEST_CASE("configured keys survive reloads and scope tool principals", "[config][auth]") {
    Directory directory;
    const auto file = directory.path / "config.json";
    auto blank = Config::load(file);
    CHECK_FALSE(blank.authenticate(""));
    CHECK_FALSE(blank.authenticate("unconfigured"));
    CHECK(blank.to_json().at("api_key") == "");
    const std::string key = "synthetic-config-key-" + random_hex(24);
    atomic_write(file, json::serialize(json::object{{"api_key", key}}), true);
    auto configured = Config::load(file);
    REQUIRE(configured.authenticate(key));
    CHECK_FALSE(configured.authenticate(""));
    CHECK_FALSE(configured.authenticate(key + "wrong"));
    CHECK(configured.authenticate(key)->find(key) == std::string::npos);
    atomic_write(file, json::serialize(configured.to_json(true)), true);
    CHECK(Config::load(file).authenticate(key) == configured.authenticate(key));
    auto rotated = configured;
    rotated.api_key += "-rotated";
    CHECK(rotated.authenticate(rotated.api_key) != configured.authenticate(key));
    CHECK_FALSE(rotated.authenticate(key));
    for (const auto &invalid : json::array{false, 7, json::array{}, json::object{},
                 "with whitespace", "with\ncontrol", std::string(4097, 'x')}) {
        try {
            (void)Config::load(file, {{"api_key", invalid}});
            FAIL("Invalid API key was accepted");
        } catch (const Error &error) {
            CHECK(error.code == "invalid_config");
            CHECK(std::string(error.what()).find(key) == std::string::npos);
        }
    }
}
TEST_CASE("Cookie import keys are separate and their limits are validated", "[config][auth]") {
    Directory directory;
    auto cfg = Config::load(directory.path / "config.json",
        {{"api_key", "generation-test-key"}, {"cookie_import_key", "import-test-key"}});
    CHECK(cfg.authenticate("generation-test-key"));
    CHECK_FALSE(cfg.authenticate("import-test-key"));
    CHECK(cfg.authenticate_cookie_import("import-test-key"));
    CHECK_FALSE(cfg.authenticate_cookie_import("generation-test-key"));
    CHECK_FALSE(cfg.authenticate_cookie_import(""));
    CHECK(cfg.authenticate("generation-test-key") != cfg.authenticate_cookie_import("import-test-key"));
    for (const auto &invalid : std::vector<json::object>{
             {{"cookie_import_key", 7}}, {{"cookie_import_key", "invalid key"}},
             {{"cookie_import_max_accounts", 0}}, {{"cookie_import_max_accounts", 33}},
             {{"cookie_import_timeout_seconds", 0}}, {{"cookie_import_timeout_seconds", 241}}})
        CHECK_THROWS_AS(Config::load(directory.path / "config.json", invalid), Error);
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
