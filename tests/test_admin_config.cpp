// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/management.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
struct AdminDirectory {
    fs::path path = fs::temp_directory_path() / ("ais2api-admin-config-" + random_hex(8));
    AdminDirectory() { fs::create_directories(path); }
    ~AdminDirectory() { std::error_code ec; fs::remove_all(path, ec); }
};
}
TEST_CASE("management config authentication and origins are independent", "[config][admin]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    CHECK_FALSE(cfg.authenticate_admin(""));
    cfg = cfg.patched({{"api_key", "api-test"}, {"admin_key", "admin-test"},
                       {"admin_origins", json::array{"http://localhost:8080", "https://example.com", "http://[::1]:3000"}}});
    CHECK(cfg.authenticate_admin("admin-test"));
    CHECK_FALSE(cfg.authenticate_admin("api-test"));
    CHECK_FALSE(cfg.authenticate("admin-test"));
    CHECK(cfg.to_json().at("admin_origins") == cfg.admin_origins);
    Store store(dir.path / "data");
    auto shown = manage(store, cfg, {{"command", "config.show"}});
    CHECK(shown.at("config").as_object().at("admin_key") == "***");
    for (const auto& value : json::array{7, true, json::object{}, "https://example.com/", "https://user@example.com", "https://*.example.com", "https://example.com?x", "https://example.com#x", "ftp://example.com", "https://example.com:99999", "https://example.com:", "https://bad..example"})
        CHECK_THROWS_AS(cfg.patched({{"admin_origins", json::array{value}}}), Error);
    CHECK_THROWS_AS(cfg.patched({{"admin_origins", "https://example.com"}}), Error);
    CHECK_THROWS_AS(cfg.patched({{"admin_key", "with space"}}), Error);
    CHECK_THROWS_AS(cfg.patched({{"admin_key", 7}}), Error);
}
TEST_CASE("management config patches validate without file changes", "[config][admin]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    cfg.model_aliases = {{"old", "model"}};
    cfg.model_defaults = {{"old", 1}};
    auto patched = cfg.patched({{"limits", json::object{{"queue_size", 12}}},
        {"model_aliases", json::object{{"new", "replacement"}}}, {"model_defaults", json::object{}}});
    CHECK(patched.limits.queue_size == 12);
    CHECK(patched.limits.request_bytes == cfg.limits.request_bytes);
    CHECK_FALSE(patched.model_aliases.contains("old"));
    CHECK(patched.model_defaults.empty());
    CHECK_FALSE(fs::exists(cfg.config_file));
    CHECK_FALSE(restart_required(cfg, patched));
    CHECK(restart_required(cfg, cfg.patched({{"port", 18000}})));
    for (const auto& patch : std::vector<json::object>{
        {{"unknown", 1}}, {{"limits", json::object{{"unknown", 1}}}},
        {{"limits", json::object{{"queue_size", "12"}}}}, {{"limits", json::object{{"queue_size", 0}}}},
        {{"limits", false}}, {{"headless", 1}}, {{"model_aliases", json::object{{"x", 1}}}},
        {{"api_key", "***"}}, {{"cookie_import_key", "***"}}, {{"admin_key", "***"}}})
        CHECK_THROWS_AS(cfg.patched(patch), Error);
}
TEST_CASE("account metadata updates validate input and unknown ids", "[management][admin]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    Store store(dir.path / "data");
    CHECK_THROWS_AS(manage(store, cfg, {{"command", "accounts.update"}, {"id", "missing"}, {"name", "updated"}}), Error);
    Account account;
    account.id = "test-account";
    account.name = "before";
    account.storage_state = {{"cookies", json::array{json::object{{"name", "SID"},
        {"value", "synthetic"}, {"domain", ".google.com"}, {"path", "/"}, {"secure", true}}}},
        {"origins", json::array{}}};
    store.put_account(account);
    auto result = manage(store, cfg, {{"command", "accounts.update"}, {"id", account.id}, {"name", "after"}, {"disabled", true}});
    CHECK(result.at("ok") == true);
    CHECK_FALSE(result.contains("storage_state"));
    CHECK(store.account(account.id).name == "after");
    CHECK(store.account(account.id).disabled);
    for (const auto& patch : std::vector<json::object>{ {{"name", 7}}, {{"disabled", "false"}}, {{"storage_state", json::object{}}} }) {
        auto command = patch; command["command"] = "accounts.update"; command["id"] = account.id;
        CHECK_THROWS_AS(manage(store, cfg, command), Error);
    }
}
