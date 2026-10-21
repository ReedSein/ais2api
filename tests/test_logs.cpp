// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/logs.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <thread>
using namespace ais2api;
namespace {
struct LogDirectory {
    fs::path path = fs::temp_directory_path() / ("ais2api-logs-" + random_hex(8));
    ~LogDirectory() { std::error_code error; fs::remove_all(path, error); }
};
}
TEST_CASE("Journal persists all history and pages by byte cursor", "[logs]") {
    LogDirectory dir;
    {
        logs::Journal journal(dir.path);
        for (int i = 0; i < 510; ++i) journal.append({{"event", "test"}, {"sequence", i}});
    }
    logs::Journal journal(dir.path);
    auto first = journal.read("0", 500);
    REQUIRE(first.at("entries").as_array().size() == 500);
    CHECK(first.at("has_more").as_bool());
    auto next = string_or(first, "next_cursor");
    auto last = journal.read(next, 500);
    CHECK(last.at("entries").as_array().size() == 10);
    CHECK_FALSE(last.at("has_more").as_bool());
    const auto &record = first.at("entries").as_array()[0].as_object().at("record").as_object();
    CHECK(record.contains("timestamp"));
    CHECK(record.contains("id"));
    CHECK(journal.read(string_or(last, "next_cursor")).at("entries").as_array().empty());
    for (auto cursor : {"-1", "x", "1", "999999999", "18446744073709551616", ""}) {
        try { journal.read(cursor); FAIL("invalid cursor accepted"); }
        catch (const Error &error) { CHECK(error.status == 400); }
    }
    CHECK_THROWS_AS(journal.read("0", 0), Error);
    CHECK_THROWS_AS(journal.read("0", 501), Error);
}
TEST_CASE("Journal serializes concurrent append operations", "[logs]") {
    LogDirectory dir;
    logs::Journal journal(dir.path);
    std::vector<std::jthread> workers;
    std::atomic<int> failures{0};
    for (int worker = 0; worker < 4; ++worker)
        workers.emplace_back([&, worker] {
            try { for (int i = 0; i < 20; ++i) journal.append({{"worker", worker}, {"sequence", i}}); }
            catch (...) { ++failures; }
        });
    workers.clear();
    CHECK(failures == 0);
    CHECK(journal.read().at("entries").as_array().size() == 80);
}
TEST_CASE("Journal rejects unsafe paths oversized entries and incomplete records", "[logs]") {
    LogDirectory dir;
    logs::Journal journal(dir.path);
    CHECK_THROWS_AS(journal.append({{"message", std::string(logs::Journal::max_record_bytes, 'x')}}), Error);
    journal.append({{"event", "complete"}});
    auto first = journal.read();
    {
        std::ofstream out(dir.path / "management.log", std::ios::binary | std::ios::app);
        out << "{\"event\":\"partial";
    }
    auto partial = journal.read();
    CHECK(partial.at("entries").as_array().size() == 1);
    CHECK(string_or(partial, "next_cursor") == string_or(first, "next_cursor"));
    CHECK_FALSE(partial.at("has_more").as_bool());
    CHECK_THROWS_AS(journal.append({{"event", "later"}}), Error);
    fs::remove(dir.path / "management.log");
    fs::create_directory(dir.path / "management.log");
    CHECK_THROWS_AS(journal.read(), Error);
    CHECK_THROWS_AS(journal.append({{"event", "failed"}}), Error);
}
TEST_CASE("Journal refuses symlink files", "[logs]") {
    LogDirectory dir;
    secure_directory(dir.path);
    atomic_write(dir.path / "target", "");
    std::error_code error;
    fs::create_symlink(dir.path / "target", dir.path / "management.log", error);
    if (error) { SUCCEED("Symlink creation unavailable for this account"); return; }
    CHECK_THROWS_AS(logs::Journal(dir.path), Error);
}
TEST_CASE("Journal bounds page bytes without losing complete records", "[logs]") {
    LogDirectory dir;
    logs::Journal journal(dir.path);
    for (int i = 0; i < 10; ++i)
        journal.append({{"sequence", i}, {"message", std::string(60000, 'x')}});
    std::string cursor = "0";
    std::size_t total = 0;
    bool more = true;
    while (more) {
        auto page = journal.read(cursor, 500);
        auto count = page.at("entries").as_array().size();
        REQUIRE(count > 0);
        CHECK(count <= 4);
        for (const auto& entry : page.at("entries").as_array())
            CHECK(entry.as_object().at("record").as_object().at("sequence").as_int64() == static_cast<std::int64_t>(total++));
        cursor = string_or(page, "next_cursor");
        more = bool_or(page, "has_more");
    }
    CHECK(total == 10);
}
