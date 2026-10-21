// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/cookie_import.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
struct ImportBackend : GenerationBackend {
    std::vector<json::object> replies;
    std::vector<Account> candidates;
    int fail_at = -1;
    bool closed = false;
    bool expand_state = false;
    std::string failure_code = "secret";
    void generate(const Request &, const Account &, const EventSink &, std::stop_token) override {}
    json::array models(std::stop_token = {}) override { return {}; }
    json::object probe(const Account &, std::stop_token = {}) override { return {}; }
    json::object probe_import(Account &a, std::stop_token token = {}) override {
        check_cancel(token);
        candidates.push_back(a);
        if (expand_state) a.storage_state.at("cookies").as_array()[0].as_object()["value"] = std::string(1024 * 1024, 'x');
        if (static_cast<int>(candidates.size()) == fail_at)
            throw Error(503, failure_code, "private-cookie-backend-secret");
        if (candidates.size() > replies.size()) throw Error(401, "login", "private-cookie");
        return replies[candidates.size() - 1];
    }
    json::object diagnostics() override { return {}; }
    void close() override { closed = true; }
};
json::object verified(std::string email) {
    return {{"ready", true}, {"login", false}, {"identityAmbiguous", false}, {"email", email}};
}
json::object request() { return {{"cookies", "SID=private-cookie; __Host-ONE=skip; OSID=host-cookie"}}; }
struct Temp {
    fs::path path = fs::temp_directory_path() / ("ais-import-" + random_hex(8));
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
}
TEST_CASE("cookie push enumerates verified identities and safely scopes raw cookies") {
    ImportBackend backend;
    backend.replies = {verified("one@example.com"), verified("two@example.com"), verified("ONE@example.com")};
    auto result = probe_cookie_import(request(), 8, backend);
    REQUIRE(result.accounts.size() == 2);
    CHECK(result.details.at("attempted") == 3);
    CHECK(result.details.at("skipped_host_cookies") == 1);
    CHECK_FALSE(backend.closed);
    auto cookies = result.accounts[0].storage_state.at("cookies").as_array();
    REQUIRE(cookies.size() == 2);
    CHECK(cookies[1].as_object().at("domain") == "aistudio.google.com");
    CHECK(result.accounts[1].auth_user == "1");
}
TEST_CASE("structured cookie push preserves exact metadata and explicit identity") {
    ImportBackend backend;
    backend.replies = {verified("one@example.com")};
    json::object cookie{{"name", "SID"}, {"value", "private-cookie"}, {"domain", "accounts.google.com"},
                        {"path", "/specific"}, {"secure", false}, {"httpOnly", false}, {"sameSite", "Strict"}, {"expires", 1234}};
    json::object input{{"cookies", json::array{cookie}}, {"auth_user", "7"}, {"email", "ONE@example.com"}};
    auto result = probe_cookie_import(input, 8, backend);
    REQUIRE(result.accounts.size() == 1);
    CHECK(result.accounts[0].auth_user == "7");
    CHECK(result.accounts[0].storage_state.at("cookies").as_array()[0] == cookie);
    cookie["partitionKey"] = "https://google.com";
    CHECK_THROWS(probe_cookie_import({{"cookies", json::array{cookie}}}, 8, backend));
    cookie.erase("partitionKey"); cookie["firstPartyDomain"] = "google.com";
    CHECK_THROWS(probe_cookie_import({{"cookies", json::array{cookie}}}, 8, backend));
    cookie.erase("firstPartyDomain"); cookie["domain"] = "evilgoogle.com";
    CHECK_THROWS(probe_cookie_import({{"cookies", json::array{cookie}}}, 8, backend));
}
TEST_CASE("legacy userscript header keeps Google RPC authentication cookies script readable") {
    ImportBackend backend; backend.replies = {verified("one@example.com")};
    auto result = probe_cookie_import({{"cookies", "SID=s; APISID=a; SAPISID=p; __Secure-1PAPISID=one; __Secure-3PAPISID=three; HSID=h; SSID=t"}, {"auth_user", "0"}}, 8, backend);
    const auto &cookies = result.accounts[0].storage_state.at("cookies").as_array();
    REQUIRE(cookies.size() == 7);
    for (std::size_t i = 0; i < cookies.size(); ++i) {
        CHECK(bool_or(cookies[i].as_object(), "httpOnly") == (i >= 5));
        CHECK(bool_or(cookies[i].as_object(), "secure"));
        CHECK(cookies[i].as_object().at("domain") == ".google.com");
    }
    backend.candidates.clear();
    auto exact = json::object{{"name", "SAPISID"}, {"value", "synthetic"}, {"domain", ".google.com"}, {"httpOnly", true}};
    result = probe_cookie_import({{"cookies", json::array{exact}}, {"auth_user", "0"}}, 8, backend);
    CHECK(bool_or(result.accounts[0].storage_state.at("cookies").as_array()[0].as_object(), "httpOnly"));
}
TEST_CASE("cookie import distinguishes safe browser failures without leaking exception text") {
    for (const auto &[code, status] : std::vector<std::pair<std::string, int>>{
        {"browser_not_configured", 503}, {"browser_launch", 502}, {"browser_timeout", 504},
        {"browser_disconnected", 502}, {"browser_incompatible", 502}}) {
        ImportBackend backend; backend.fail_at = 1; backend.failure_code = code;
        try { probe_cookie_import(request(), 8, backend); FAIL("expected failure"); }
        catch (const Error &e) {
            CHECK(e.code == "cookie_" + code); CHECK(e.status == status);
            CHECK(std::string(e.what()).find("private-cookie") == std::string::npos);
        }
    }
}
TEST_CASE("cookie push rejects ambiguous identities infrastructure errors and cancellation atomically") {
    Temp temp; Store store(temp.path);
    ImportBackend backend;
    backend.replies = {verified("one@example.com"), verified("")};
    CHECK_THROWS(probe_cookie_import(request(), 8, backend));
    CHECK(store.accounts().empty());
    backend.candidates.clear(); backend.fail_at = 2;
    try { probe_cookie_import(request(), 8, backend); FAIL("expected failure"); }
    catch (const Error &e) { CHECK(e.code == "cookie_probe_failed"); CHECK(std::string(e.what()).find("private-cookie") == std::string::npos); }
    CHECK(store.accounts().empty());
    std::stop_source stop; stop.request_stop();
    try { probe_cookie_import(request(), 8, backend, stop.get_token()); FAIL("expected cancellation"); }
    catch (const Error &e) { CHECK(e.code == "cancelled"); }
    CHECK_THROWS(probe_cookie_import({{"cookies", "SID=x"}, {"unknown", true}}, 8, backend));
    CHECK_THROWS(probe_cookie_import(request(), 0, backend));
}
TEST_CASE("verified account merge reuses email identity and preserves cooldown selection and stats") {
    Temp temp; Store store(temp.path);
    ImportBackend backend; backend.replies = {verified("one@example.com")};
    auto result = probe_cookie_import({{"cookies", "SID=private-cookie"}, {"auth_user", "0"}}, 8, backend);
    auto first = result.accounts[0]; first.name = "Personal name";
    store.merge_verified_accounts({first}); store.select_account(first.id);
    store.success(first.id, "model", {{"tokens", 2}});
    store.failure(first.id, "model", "429", 120);
    auto refreshed = first; refreshed.id = "different-id"; refreshed.email = "ONE@example.com";
    refreshed.name = "Automatic name"; refreshed.auth_user = "3";
    auto merged = store.merge_verified_accounts({refreshed});
    REQUIRE(store.accounts().size() == 1);
    CHECK(string_or(merged[0].as_object(), "id") == first.id);
    CHECK(store.account(first.id).name == "Personal name");
    CHECK(store.account(first.id).auth_user == "3");
    CHECK_FALSE(store.choose("model").has_value());
    CHECK(store.stats().at("records").as_array().size() == 1);
    auto bad = refreshed; bad.email.clear();
    CHECK_THROWS(store.merge_verified_accounts({refreshed, bad}));
    CHECK(store.accounts().size() == 1);
    auto unrelated = first; unrelated.email = "two@example.com";
    store.merge_verified_accounts({unrelated});
    CHECK(store.accounts().size() == 2);
}

TEST_CASE("cookie import bounds normalized and refreshed credentials and sanitizes partition counts") {
    ImportBackend backend;
    auto reply = verified("one@example.com"); reply["skippedPartitionCookies"] = 3;
    reply["untrusted"] = "private-cookie";
    backend.replies = {reply};
    auto input = request(); input["auth_user"] = "0";
    auto result = probe_cookie_import(input, 8, backend);
    CHECK(result.details.at("skipped_partition_cookies") == 3);
    CHECK(json::serialize(result.details).find("private-cookie") == std::string::npos);
    backend.candidates.clear(); backend.expand_state = true;
    try { probe_cookie_import(input, 8, backend); FAIL("expected credential size limit"); }
    catch (const Error &e) { CHECK(e.status == 413); CHECK(e.code == "cookie_import_limit"); }
    std::string header;
    for (unsigned i = 0; i < 513; ++i) header += "SID" + std::to_string(i) + "=x;";
    try { probe_cookie_import({{"cookies", header}}, 8, backend); FAIL("expected cookie count limit"); }
    catch (const Error &e) { CHECK(e.status == 413); CHECK(e.code == "cookie_import_limit"); }
    backend.expand_state = false;
    for (auto value : {json::value(-1), json::value(2.5), json::value("private-cookie")}) {
        backend.candidates.clear(); backend.replies[0]["skippedPartitionCookies"] = value;
        CHECK(probe_cookie_import(input, 8, backend).details.at("skipped_partition_cookies") == 0);
    }
}
