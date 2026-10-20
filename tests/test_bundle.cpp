// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/bundle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_predicate.hpp>
using namespace ais2api;
namespace {
struct BundleFixture {
    fs::path program = fs::temp_directory_path() / ("ais2api-bundle-" + random_hex(8));
    std::string host;
    fs::path root, executable;
    json::object marker;
    BundleFixture() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        host = "windows-x64";
#elif defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
        host = "linux-x64";
#else
        return;
#endif
        root = program / "browser-runtime" / host;
        executable = root / (host == "windows-x64" ? "chrome.exe" : "chrome");
        fs::create_directories(root);
        atomic_write(executable, "synthetic browser");
        atomic_write(root / "BINARY-LICENSE.md", "synthetic binary license");
        const auto locked_hash = sha256("locked archive");
        atomic_write(program / "runtime-manifest.json", json::serialize(json::object{
            {"version", 1}, {"browser_version", "test-version"},
            {"artifacts", json::object{{host, json::object{{"sha256", locked_hash}}}}}}));
        marker = {{"platform", host}, {"browser_version", "test-version"}, {"sha256", locked_hash},
                  {"usage", "local-internal"}, {"files", json::object{
                      {executable.filename().string(), sha256("synthetic browser")},
                      {"BINARY-LICENSE.md", sha256("synthetic binary license")}}}};
        save();
    }
    void save() { atomic_write(root / ".ais2api-runtime.json", json::serialize(marker)); }
    ~BundleFixture() { std::error_code error; fs::remove_all(program, error); }
};
bool invalid_bundle(const std::exception &exception) {
    const auto *error = dynamic_cast<const Error *>(&exception);
    return error && error->code == "invalid_browser_bundle";
}
}
TEST_CASE("browser bundle discovery handles absent and locked bundles") {
    BundleFixture fixture;
    if (fixture.host.empty()) { CHECK(bundled_browser(fixture.program).empty()); return; }
    CHECK(bundled_browser(fixture.program) == fixture.executable);
    CHECK_NOTHROW(verify_browser_bundle(fixture.executable));
    fs::remove_all(fixture.root);
    CHECK(bundled_browser(fixture.program).empty());
}
TEST_CASE("browser bundle discovery rejects mismatched ownership") {
    BundleFixture fixture;
    if (fixture.host.empty()) return;
    SECTION("version") { fixture.marker["browser_version"] = "different"; }
    SECTION("platform") { fixture.marker["platform"] = "termux-arm64"; }
    SECTION("archive hash") { fixture.marker["sha256"] = sha256("different archive"); }
    SECTION("unsafe inventory") { fixture.marker.at("files").as_object()["../outside"] = sha256("outside"); }
    SECTION("Windows device alias") { fixture.marker.at("files").as_object()["CON.txt"] = sha256("device"); }
    SECTION("trailing dot alias") { fixture.marker.at("files").as_object()["chrome."] = sha256("alias"); }
    SECTION("private inventory") { fixture.marker.at("files").as_object()["profile/Cookies"] = sha256("private"); }
    fixture.save();
    CHECK_THROWS_MATCHES(bundled_browser(fixture.program), std::exception,
        Catch::Matchers::Predicate<std::exception>(invalid_bundle));
}
TEST_CASE("full browser bundle validation rejects tampering and extra private files") {
    BundleFixture fixture;
    if (fixture.host.empty()) return;
    SECTION("tampered executable") { atomic_write(fixture.executable, "changed"); }
    SECTION("extra file") { atomic_write(fixture.root / "extra.txt", "extra"); }
    SECTION("missing license") { fs::remove(fixture.root / "BINARY-LICENSE.md"); }
    SECTION("private path") { fs::create_directories(fixture.root / "profile"); atomic_write(fixture.root / "profile" / "Cookies", "private"); }
    CHECK_THROWS_MATCHES(verify_browser_bundle(fixture.executable), std::exception,
        Catch::Matchers::Predicate<std::exception>(invalid_bundle));
}
TEST_CASE("browser bundle rejects symlink executables and markers when supported") {
    BundleFixture fixture;
    if (fixture.host.empty()) return;
    auto target = fixture.program / "outside";
    atomic_write(target, "outside");
    fs::path link;
    SECTION("executable") { link = fixture.executable; }
    SECTION("marker") { link = fixture.root / ".ais2api-runtime.json"; }
    fs::remove(link);
    std::error_code error;
    fs::create_symlink(target, link, error);
    if (error) return; // Windows accounts may lack symlink creation privileges.
    CHECK_THROWS_MATCHES(bundled_browser(fixture.program), std::exception,
        Catch::Matchers::Predicate<std::exception>(invalid_bundle));
}
