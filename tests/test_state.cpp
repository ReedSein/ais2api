// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/store.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sqlite3.h>
#include <thread>
using namespace ais2api;
namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-test-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
json::object credential(std::string value = "private-cookie") {
    return {{"cookies", json::array{json::object{{"name", "SID"},
                                                 {"value", value},
                                                 {"domain", ".google.com"},
                                                 {"path", "/"},
                                                 {"secure", true}}}},
            {"origins", json::array{}}};
}
} // namespace
TEST_CASE("configuration merges partial limits and rejects unsafe types") {
    Directory d;
    auto file = d.path / "config.json";
    atomic_write(file, json::serialize(json::object{
                           {"version", 1}, {"port", 1234}, {"limits", {{"queue_size", 2}}}}));
    auto config = Config::load(file, {{"port", 4321}});
    CHECK(config.port == 4321);
    CHECK(config.limits.queue_size == 2);
    CHECK(config.limits.request_bytes == 32 * 1024 * 1024);
    CHECK(config.to_json().at("version") == 1);
    CHECK_THROWS(Config::load(file, {{"port", 65536}}));
    CHECK_THROWS(Config::load(file, {{"headless", "false"}}));
    CHECK_THROWS(Config::load(file, {{"version", 2}}));
    CHECK_THROWS(Config::load(file, {{"unknown", true}}));
    CHECK_THROWS(Config::load(file, {{"limits", {{"max_attempts", 0}}}}));
    CHECK_THROWS(Config::load(file, {{"limits", {{"unknown", 1}}}}));
}
TEST_CASE("account imports are transactional and output excludes credentials") {
    Directory d;
    Store store(d.path);
    auto imported = store.import_accounts(credential());
    REQUIRE(imported.size() == 1);
    auto id = string_or(imported[0].as_object(), "id");
    CHECK(json::serialize(imported).find("private-cookie") == std::string::npos);
    CHECK(store.account(id).storage_state.at("cookies").as_array().size() == 1);
    json::object bundle{
        {"accounts",
         json::array{json::object{{"name", "good"}, {"cookies", credential().at("cookies")}},
                     json::object{{"name", "bad"}, {"cookies", json::array{}}}}}};
    CHECK_THROWS(store.import_accounts(bundle));
    CHECK(store.accounts().size() == 1);
    CHECK(json::serialize(store.account_status()).find("private-cookie") == std::string::npos);
    CHECK_THROWS(store.import_accounts(json::object{
        {"cookies",
         json::array{json::object{{"name", "SID"}, {"value", "secret"}, {"domain", "../bad"}}}}}));
    auto raw = store.import_accounts(json::string("SID=header-value; HSID=second"));
    CHECK(raw.size() == 1);
}
TEST_CASE("routing isolates account expiry and model permissions") {
    Directory d;
    Store store(d.path);
    auto x = store.import_accounts(credential());
    auto y = store.import_accounts(credential("second"));
    auto a = string_or(x[0].as_object(), "id"), b = string_or(y[0].as_object(), "id");
    store.select_account(a);
    REQUIRE(store.choose("m"));
    CHECK(store.choose("m")->id == a);
    store.success(b, "m", {{"total_tokens", 3}});
    CHECK(store.choose("m")->id == b);
    store.failure(b, "m", "permission");
    CHECK(store.choose("m")->id == a);
    store.select_account(b);
    CHECK(store.choose("other")->id == b);
    store.failure(b, "other", "login_expired");
    CHECK(store.choose("other")->id == a);
    store.reset_account(b);
    CHECK(store.choose("other")->id == b);
    store.failure(b, "other", "429");
    CHECK(store.choose("another")->id == a);
    CHECK_FALSE(store.choose("m", {a, b}));
    store.remove_account(a);
    CHECK_THROWS(store.account(a));
}
TEST_CASE("hashed API keys persist and cannot be enumerated in plaintext") {
    Directory d;
    std::string secret, id;
    {
        Store store(d.path);
        auto k = store.create_key("test");
        secret = string_or(k, "secret");
        id = string_or(k, "id");
        REQUIRE(secret.size() > 60);
        CHECK(store.authenticate(secret) == id);
        CHECK_FALSE(store.authenticate("incorrect"));
        CHECK(json::serialize(store.keys()).find(secret) == std::string::npos);
    }
    {
        Store store(d.path);
        CHECK(store.authenticate(secret) == id);
        store.revoke_key(id);
        CHECK_FALSE(store.authenticate(secret));
    }
}
TEST_CASE("tool metadata has principal isolation and bounded LRU eviction") {
    Directory d;
    Store store(d.path);
    store.put_tool("a", "shared", {{"signature", "a-value"}});
    store.put_tool("b", "shared", {{"signature", "b-value"}});
    CHECK(string_or(*store.get_tool("a", "shared"), "signature") == "a-value");
    CHECK_FALSE(store.get_tool("c", "shared"));
    for (int i = 0; i < 4095; ++i)
        store.put_tool("a", std::to_string(i), {{"i", i}});
    CHECK_FALSE(store.get_tool("b", "shared"));
    // Reading a refreshed its LRU position; b is the first evicted entry.
    // Insert once more without touching a to prove its eventual eviction too.
    store.put_tool("a", "additional", {{"i", 4095}});
    CHECK_FALSE(store.get_tool("a", "shared"));
    CHECK(store.get_tool("a", "4094"));
}
TEST_CASE("SQLite backup restores account statistics and key authentication") {
    Directory d;
    auto backup = d.path / "snapshot.sqlite3";
    std::string key;
    {
        Store store(d.path / "original");
        auto a = store.import_accounts(credential());
        auto id = string_or(a[0].as_object(), "id");
        store.success(id, "model", {{"total_tokens", 7}});
        key = string_or(store.create_key("backup"), "secret");
        store.backup(backup);
        CHECK_THROWS(store.backup(backup));
    }
    fs::create_directories(d.path / "restored");
    fs::copy_file(backup, d.path / "restored" / "state.sqlite3");
    Store restored(d.path / "restored");
    CHECK(restored.accounts().size() == 1);
    CHECK(restored.authenticate(key));
    CHECK(restored.stats().at("records").as_array().size() == 1);
}
TEST_CASE("store serializes simultaneous callers") {
    Directory d;
    Store store(d.path);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            for (int j = 0; j < 8; ++j)
                store.import_accounts(credential());
        });
    for (auto &t : threads)
        t.join();
    CHECK(store.accounts().size() == 32);
}
TEST_CASE("tool TTL uses original insertion time and statistics aggregate safely") {
    Directory d;
    {
        Store store(d.path);
        store.put_tool("principal", "old", {{"signature", "private"}});
        auto imported = store.import_accounts(credential());
        auto id = string_or(imported[0].as_object(), "id");
        store.success(id, "m", {{"total_tokens", 7}, {"cookie", "do-not-persist"}});
        store.success(id, "m", {{"total_tokens", 4}});
        auto records = store.stats().at("records").as_array();
        CHECK(records[0].as_object().at("usage").as_object().at("total_tokens") == 11);
        CHECK(json::serialize(records).find("do-not-persist") == std::string::npos);
    }
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "UPDATE tools SET created=0", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    Store store(d.path);
    CHECK_FALSE(store.get_tool("principal", "old"));
}
TEST_CASE("Netscape cookie metadata and JSON bundle formats import") {
    Directory d;
    Store store(d.path);
    json::string netscape("# Netscape HTTP Cookie "
                          "File\n#HttpOnly_.google.com\tTRUE\t/\tTRUE\t0\tSID\tnetscape-secret\n");
    auto imported = store.import_accounts(netscape);
    auto id = string_or(imported[0].as_object(), "id");
    auto cookie = store.account(id).storage_state.at("cookies").as_array()[0].as_object();
    CHECK(cookie.at("httpOnly").as_bool());
    CHECK(cookie.at("secure").as_bool());
    auto nested = store.import_accounts(json::object{
        {"profiles",
         json::array{json::object{{"name", "profile"},
                                  {"auth_user", "2"},
                                  {"cookies", json::string(json::serialize(credential()))}}}}});
    CHECK(nested[0].as_object().at("auth_user") == "2");
}
TEST_CASE("configuration precedence is defaults then file then environment "
          "then CLI") {
    Directory d;
    auto path = d.path / "config.json";
    atomic_write(path,
                 json::serialize(json::object{{"port", 12000}, {"limits", {{"queue_size", 3}}}}));
    const char *old = std::getenv("AIS2API_PORT");
    std::optional<std::string> previous;
    if (old)
        previous = old;
#ifdef _WIN32
    _putenv_s("AIS2API_PORT", "12001");
#else
    setenv("AIS2API_PORT", "12001", 1);
#endif
    try {
        CHECK(Config::load(path).port == 12001);
        CHECK(Config::load(path, {{"port", 12002}}).port == 12002);
    } catch (...) {
#ifdef _WIN32
        _putenv_s("AIS2API_PORT", previous ? previous->c_str() : "");
#else
        if (previous)
            setenv("AIS2API_PORT", previous->c_str(), 1);
        else
            unsetenv("AIS2API_PORT");
#endif
        throw;
    }
#ifdef _WIN32
    _putenv_s("AIS2API_PORT", previous ? previous->c_str() : "");
#else
    if (previous)
        setenv("AIS2API_PORT", previous->c_str(), 1);
    else
        unsetenv("AIS2API_PORT");
#endif
}
TEST_CASE("newer database versions are refused without altering the version") {
    Directory d;
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "PRAGMA user_version=2", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    CHECK_THROWS_AS(Store(d.path), Error);
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(sqlite3_column_int(statement, 0) == 2);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}
TEST_CASE("Cookie header imports trim only outer pipeline whitespace", "[state][cookie-text]") {
    Directory directory;
    Store store(directory.path);
    for (const auto &text : std::vector<std::string>{
             "SAPISID=synthetic\n", "SAPISID=synthetic\r\n", " \t\r\nSAPISID=synthetic \t\r\n",
             " \tCookie: SAPISID=synthetic\r\n",
             std::string("\xEF\xBB\xBF") + "SAPISID=synthetic\r\n"}) {
        auto imported = store.import_accounts(json::string(text));
        REQUIRE(imported.size() == 1);
        auto account = store.account(string_or(imported[0].as_object(), "id"));
        auto &cookies = account.storage_state.at("cookies").as_array();
        REQUIRE(cookies.size() == 1);
        CHECK(cookies[0].as_object().at("name") == "SAPISID");
        CHECK(cookies[0].as_object().at("value") == "synthetic");
    }
}
TEST_CASE("Cookie header imports reject embedded line breaks without dropping fragments",
          "[state][cookie-text]") {
    Directory directory;
    Store store(directory.path);
    for (const auto &text : std::vector<std::string>{
             "SAPISID=good\nbad\n", "SAPISID=good\r\nInjected=value\r\n",
             "SAPISID=good;\nInjected-Header: evil\n", "SAPISID=good\rIgnored\r\n"}) {
        try {
            store.import_accounts(json::string(text));
            FAIL("Embedded header newline was accepted");
        } catch (const Error &error) {
            CHECK(error.status == 400);
            CHECK(error.code == "invalid_credentials");
        }
    }
    CHECK(store.accounts().empty());
}
TEST_CASE("BOM JSON credentials and Netscape row delimiters remain intact",
          "[state][cookie-text]") {
    Directory directory;
    Store store(directory.path);
    auto json_import = store.import_accounts(json::string(
        std::string("\xEF\xBB\xBF") +
        json::serialize(json::object{
            {"accounts", json::array{json::object{{"name", "BOM bundle"},
                                                  {"storage_state", credential("json-value")}}}}}) +
        "\r\n"));
    REQUIRE(json_import.size() == 1);
    CHECK(store.account(string_or(json_import[0].as_object(), "id"))
              .storage_state.at("cookies")
              .as_array()[0]
              .as_object()
              .at("value") == "json-value");
    auto netscape = store.import_accounts(
        json::string("# Netscape HTTP Cookie "
                     "File\r\n.google.com\tTRUE\t/"
                     "\tTRUE\t0\tSID\tfirst-value\r\n.google.com\tTRUE\t/\tTRUE\t0\tEMPTY\t\r\n"));
    auto saved = store.account(string_or(netscape[0].as_object(), "id"))
                     .storage_state.at("cookies")
                     .as_array();
    REQUIRE(saved.size() == 2);
    CHECK(saved[0].as_object().at("value") == "first-value");
    CHECK(saved[1].as_object().at("value") == "");
}
