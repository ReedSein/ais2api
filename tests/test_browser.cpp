// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/browser.hpp"
#include "../browser/cdp.hpp"
#include "../browser/rpc_error.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <future>
#include <thread>

using namespace ais2api;
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

// A real loopback websocket peer, not a mocked Cdp::call implementation.
struct Peer {
    asio::io_context io;
    tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    unsigned short port = acceptor.local_endpoint().port();
    std::promise<json::object> received;
    std::future<json::object> request = received.get_future();
    std::jthread worker;
    explicit Peer(bool delayed = false) : worker([this, delayed] {
        try {
            websocket::stream<tcp::socket> ws{io};
            acceptor.accept(ws.next_layer()); ws.accept();
            beast::flat_buffer buffer; ws.read(buffer);
            auto object = json::parse(beast::buffers_to_string(buffer.data())).as_object();
            received.set_value(object);
            if (delayed) { std::this_thread::sleep_for(300ms); return; }
            const auto id = object.at("id");
            auto send = [&](const json::object& message) { auto encoded = json::serialize(message); ws.write(asio::buffer(encoded)); };
            send({{"method", "Runtime.consoleAPICalled"}, {"sessionId", "isolated-session"}, {"params", json::object{}}});
            send({{"id", 999}, {"result", {{"ignored", true}}}});
            send({{"id", id}, {"sessionId", string_or(object, "sessionId")}, {"result", {{"result", {{"type", "string"}, {"value", "real-websocket-reply"}}}}}});
        } catch (...) { try { received.set_exception(std::current_exception()); } catch (...) {} }
    }) {}
};
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-browser-test-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
}

TEST_CASE("native CDP correlates JSON RPC through interleaved session events") {
    Peer peer;
    browser_detail::Cdp client;
    client.connect(peer.port, "/devtools/browser/test-owned-browser", {});
    const auto value = client.evaluate("1 + 1", "isolated-session", {});
    REQUIRE(value.is_string()); CHECK(value.as_string() == "real-websocket-reply");
    auto request = peer.request.get();
    CHECK(string_or(request, "method") == "Runtime.evaluate");
    CHECK(string_or(request, "sessionId") == "isolated-session");
    CHECK(string_or(request.at("params").as_object(), "expression") == "1 + 1");
    CHECK(bool_or(request.at("params").as_object(), "awaitPromise"));
}

TEST_CASE("native CDP cancels a websocket read without waiting for its timeout") {
    Peer peer(true);
    browser_detail::Cdp client;
    client.connect(peer.port, "/devtools/browser/test-owned-browser", {});
    std::stop_source source;
    std::jthread cancel([&] { std::this_thread::sleep_for(60ms); source.request_stop(); });
    const auto began = Clock::now();
    try { client.evaluate("new Promise(()=>{})", "session", source.get_token(), 30s); FAIL("Expected cancellation"); }
    catch (const Error& error) { CHECK(error.code == "cancelled"); }
    CHECK(Clock::now() - began < 1s);
    CHECK(string_or(peer.request.get(), "sessionId") == "session");
}

TEST_CASE("unconfigured browser probe fails without claiming account readiness") {
    Directory directory;
    Config config; config.data_dir = directory.path;
    Store store(directory.path);
    BrowserGateway gateway(config, store);
    Account account; account.id = "test-account";
    try { gateway.probe(account); FAIL("Expected missing browser configuration"); }
    catch (const Error& error) { CHECK(error.code == "browser_not_configured"); }
    try { gateway.probe_import(account); FAIL("Expected missing browser configuration"); }
    catch (const Error& error) { CHECK(error.code == "browser_not_configured"); }
    CHECK_FALSE(bool_or(gateway.diagnostics(), "running"));
    CHECK_FALSE(bool_or(gateway.diagnostics(), "templateCaptured"));
    gateway.close(); gateway.close();
}
TEST_CASE("account verification distinguishes delayed identity from a different Google account") {
    using browser_detail::IdentityVerdict;
    using browser_detail::identity_verdict;
    json::object identity{{"ready", true}, {"identityReady", false}, {"email", ""}};
    CHECK(identity_verdict(identity, "expected@example.com") == IdentityVerdict::pending);
    identity["email"] = "expected@example.com";
    CHECK(identity_verdict(identity, "expected@example.com") == IdentityVerdict::pending);
    identity["identityReady"] = true;
    CHECK(identity_verdict(identity, "EXPECTED@example.com") == IdentityVerdict::verified);
    CHECK(identity_verdict(identity, " expected@example.com\r\n") == IdentityVerdict::verified);
    CHECK(identity_verdict(identity, "other@example.com") == IdentityVerdict::mismatch);
    identity["identityAmbiguous"] = true;
    CHECK(identity_verdict(identity, "expected@example.com") == IdentityVerdict::ambiguous);
    identity["identityAmbiguous"] = false; identity["email"] = "";
    CHECK(identity_verdict(identity, "expected@example.com") == IdentityVerdict::pending);
    identity["email"] = "one@example.com";
    CHECK(identity_verdict(identity, "") == IdentityVerdict::verified);
}

TEST_CASE("import cookie refresh retains only supported Google cookie fields") {
    json::array input{
        json::object{{"name", "SID"}, {"value", "test-only"}, {"domain", ".google.com"}, {"path", "/"}, {"secure", true}, {"httpOnly", true}, {"sameSite", "Lax"}, {"expires", 123456789.0}, {"priority", "High"}},
        json::object{{"name", "session"}, {"value", ""}, {"domain", "accounts.google.com"}, {"expires", -1}, {"sameSite", "invalid"}},
        json::object{{"name", "bad"}, {"value", "private"}, {"domain", "evilgoogle.com"}},
        json::object{{"name", "bad"}, {"value", "private"}, {"domain", "google.com.example.com"}},
        json::object{{"name", "bad"}, {"domain", "google.com"}},
        json::object{{"name", ""}, {"value", "private"}, {"domain", "google.com"}}
    };
    auto cookies = browser_detail::import_cookies(input);
    REQUIRE(cookies.size() == 2);
    const auto& persistent = cookies[0].as_object();
    CHECK(persistent.size() == 8);
    CHECK_FALSE(persistent.contains("priority"));
    CHECK_FALSE(persistent.contains("partitionKey"));
    CHECK(string_or(persistent, "domain") == ".google.com");
    const auto& session = cookies[1].as_object();
    CHECK_FALSE(session.contains("expires"));
    CHECK_FALSE(session.contains("sameSite"));
    CHECK(string_or(session, "value").empty());
}

TEST_CASE("import cookie refresh never downgrades partitioned cookies") {
    json::array input{
        json::object{{"name", "partitioned"}, {"value", "test-only"}, {"domain", ".google.com"}, {"partitionKey", json::object{{"topLevelSite", "https://google.com"}}}},
        json::object{{"name", "opaque"}, {"value", "test-only"}, {"domain", "accounts.google.com"}, {"partitionKeyOpaque", true}},
        json::object{{"name", "unknown-key"}, {"value", "test-only"}, {"domain", ".google.com"}, {"partitionKey", nullptr}},
        json::object{{"name", "__Host-test"}, {"value", "test-only"}, {"domain", "accounts.google.com"}, {"path", "/"}, {"secure", true}, {"partitionKeyOpaque", false}}
    };
    std::size_t skipped = 99;
    auto cookies = browser_detail::import_cookies(input, &skipped);
    REQUIRE(cookies.size() == 1);
    CHECK(skipped == 3);
    CHECK(string_or(cookies[0].as_object(), "name") == "__Host-test");
    CHECK(string_or(cookies[0].as_object(), "domain") == "accounts.google.com");
    CHECK(bool_or(cookies[0].as_object(), "secure"));
    browser_detail::import_cookies({}, &skipped);
    CHECK(skipped == 0);
}

// Hidden by default: explicitly select this test with a prepared executable.
// It opens only about:blank; no cookies, account probe or Google request.
TEST_CASE("installed browser verifier releases its process and profile", "[.browser-runtime]") {
    const auto executable = std::getenv("AIS2API_TEST_BROWSER");
    REQUIRE(executable != nullptr);
    REQUIRE(fs::is_regular_file(fs::u8path(executable)));
    Directory directory;
    Config config;
    config.data_dir = directory.path;
    config.browser_executable = fs::u8path(executable);
    config.headless = true;
    Store store(config.data_dir);
    BrowserGateway gateway(config, store);
    auto result = gateway.verify_runtime();
    CHECK_FALSE(string_or(result, "product").empty());
    CHECK_FALSE(string_or(result, "protocol_version").empty());
    CHECK(bool_or(result, "isolated_context"));
    REQUIRE(result.contains("google_verified"));
    CHECK_FALSE(bool_or(result, "google_verified"));
    CHECK_FALSE(bool_or(gateway.diagnostics(), "running"));
    for (const auto &entry : fs::directory_iterator(config.data_dir / "runtime"))
        CHECK_FALSE(entry.path().filename().string().starts_with("browser-"));
    bool retained_log = false;
    for (const auto &entry : fs::directory_iterator(config.data_dir / "logs"))
        retained_log = retained_log || (entry.is_regular_file() && entry.path().extension() == ".log");
    CHECK(retained_log);
}

TEST_CASE("model discovery distinguishes configured data from live Google data") {
    Directory directory;
    Config config; config.data_dir = directory.path;
    config.configured_models = {"explicit-user-model"};
    Store store(directory.path);
    BrowserGateway gateway(config, store);
    auto models = gateway.models();
    REQUIRE(models.size() == 1);
    CHECK(string_or(models[0].as_object(), "id") == "explicit-user-model");
    CHECK(string_or(models[0].as_object(), "source") == "configured");
    CHECK_FALSE(bool_or(gateway.diagnostics(), "running"));
    std::stop_source cancelled; cancelled.request_stop();
    CHECK_THROWS_AS(gateway.models(cancelled.get_token()), Error);
}

TEST_CASE("HTTP 200 RPC status envelopes retain scheduler error categories") {
    for (const auto& [wire, status] : std::vector<std::pair<std::string, int>>{
        {"[[[8,\"quota exhausted\"]]]", 429},
        {"[null,[7,\"model permission denied\"]]", 403},
        {"[,[16,\"credentials expired\"]]", 401},
        {")]}'\n[[[8,\"try later\",[]]]]", 429},
        {"{\"error\":{\"code\":429,\"message\":\"quota\"}}", 429},
        {"{\"error\":{\"status\":\"UNAUTHENTICATED\"}}", 401}
    }) {
        auto error = browser_detail::rpc_text_error(wire, 17);
        REQUIRE(error.has_value()); CHECK(error->status == status); CHECK(error->retry_after == 17);
        CHECK(std::string(error->what()).find("credentials expired") == std::string::npos);
    }
    CHECK_FALSE(browser_detail::rpc_text_error("[[[0,\"success\"]]]"));
    CHECK_FALSE(browser_detail::rpc_text_error("[[[8,\"partial"));
    auto http = browser_detail::rpc_status_error(503, 400000);
    REQUIRE(http); CHECK(http->status == 503); CHECK(http->retry_after == 86400);
}

TEST_CASE("generated tool arguments cannot masquerade as RPC errors") {
    json::array part{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     json::array{"tool", json::object{{"error", {{"code", 8}, {"message", "user data"}}}, {"tuple", json::array{16, "user value"}}}}};
    json::array content{json::array{part}, "model"};
    json::array candidate{json::value(content)};
    json::array wire{json::value(json::array{json::value(candidate)})};
    CHECK_FALSE(browser_detail::rpc_error(wire));
}

TEST_CASE("snapshot hash follows encoded contents including expanded attachments") {
    const auto wire = json::parse(R"([null,[[[[null,"thought",null,null,null,null,null,null,null,null,null,null,1],[null,""],[null,null,["image/png","YWJj"]],[null,null,null,null,null,null,null,[1,"ignored code"]],[null,null,null,null,null,null,null,null,[1,"ignored output"]],[null,null,null,null,null,null,null,null,null,null,["fn"]]],"model"]],null,null,null,[[[null,"excluded system"]],"user"]])").as_array();
    REQUIRE(browser_detail::wire_content_hash(wire) == sha256("thought  YWJj   "));
    Request request;
    request.model = "test";
    request.gemini = json::parse(R"({"contents":[{"role":"user","parts":[{"functionResponse":{"name":"fn","response":{},"parts":[{"text":"attached"},{"inlineData":{"mimeType":"image/png","data":"YWJj"}}]}}]}]})").as_object();
    REQUIRE(browser_detail::wire_content_hash(encode_request(request, {}, "")) == sha256(" attached YWJj"));
}
