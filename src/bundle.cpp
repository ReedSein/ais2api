// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/bundle.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <set>
#ifdef _WIN32
#include <windows.h>
#endif
namespace ais2api {
namespace {
constexpr const char *marker_name = ".ais2api-runtime.json";
[[noreturn]] void invalid() {
    throw Error(400, "invalid_browser_bundle", "Browser bundle is missing, unsafe, or differs from its locked inventory");
}
std::string platform() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x64";
#elif defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    return "linux-x64";
#else
    return {};
#endif
}
void no_link(const fs::path &path) {
    if (fs::is_symlink(fs::symlink_status(path))) invalid();
#ifdef _WIN32
    auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) invalid();
#endif
}
bool hex_digest(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void safe_name(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != name.npos || name.find(':') != name.npos || name.find('\0') != name.npos) invalid();
    static const std::set<std::string> forbidden{"data", "profiles", "profile", "default", "user data", "downloads", "cookies", "network", "local state", "preferences", "history", "login data", "web data", "sessions", "cache", "code cache", "gpucache", "logs", "server-logs", ".git", ".env", "config.yaml", "installation.json"};
    std::size_t start = 0;
    while (start < name.size()) {
        auto end = name.find('/', start);
        if (end == name.npos) end = name.size();
        auto part = std::string(name.substr(start, end - start));
        if (part.empty() || part == "." || part == "..") invalid();
        std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (part.back() == '.' || part.back() == ' ' || std::any_of(part.begin(), part.end(), [](unsigned char c) {
            return c < 32 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*';
        })) invalid();
        const auto device = part.substr(0, part.find('.'));
        if (device == "con" || device == "prn" || device == "aux" || device == "nul" || device == "conin$" || device == "conout$") invalid();
        if (device.size() == 4 && (device.starts_with("com") || device.starts_with("lpt")) && device[3] >= '1' && device[3] <= '9') invalid();
        if (forbidden.contains(part)) invalid();
        for (auto suffix : {".sqlite", ".sqlite3", ".db", ".log", ".dmp"})
            if (part.ends_with(suffix)) invalid();
        start = end + 1;
    }
    if (name.back() == '/') invalid();
}
json::object metadata(const fs::path &program, const fs::path &root, const std::string &host) {
    no_link(root.parent_path()); no_link(root); no_link(root / marker_name);
    if (!fs::is_directory(root) || !fs::is_regular_file(root / marker_name)) invalid();
    auto manifest = json::parse(read_file(program / "runtime-manifest.json", 65536)).as_object();
    auto marker = json::parse(read_file(root / marker_name, 4 * 1024 * 1024)).as_object();
    auto artifact = manifest.at("artifacts").as_object().at(host).as_object();
    if (manifest.at("version") != 1 || string_or(marker, "platform") != host ||
        string_or(marker, "browser_version") != string_or(manifest, "browser_version") ||
        string_or(marker, "sha256") != string_or(artifact, "sha256") ||
        !hex_digest(string_or(marker, "sha256")) || string_or(marker, "usage") != "local-internal") invalid();
    const auto &files = marker.at("files").as_object();
    const std::string executable = host == "windows-x64" ? "chrome.exe" : "chrome";
    if (files.empty() || !files.contains(executable) || !files.contains("BINARY-LICENSE.md")) invalid();
    for (const auto &file : files) {
        safe_name(std::string(file.key()));
        if (file.key() == marker_name || !file.value().is_string() || !hex_digest(std::string(file.value().as_string()))) invalid();
    }
    no_link(root / executable);
    if (!fs::is_regular_file(root / executable)) invalid();
    return marker;
}
std::string file_hash(const fs::path &file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) invalid();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) invalid();
    std::array<char, 64 * 1024> buffer;
    while (input) {
        input.read(buffer.data(), buffer.size());
        if (input.gcount() && EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(input.gcount())) != 1) invalid();
    }
    if (!input.eof()) invalid();
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context.get(), bytes.data(), &length) != 1 || length != 32) invalid();
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned int i = 0; i < length; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
    return result;
}
}
fs::path bundled_browser(const fs::path &program_directory) {
    try {
        const auto host = platform();
        if (host.empty()) return {};
        const auto program = fs::absolute(program_directory).lexically_normal();
        const auto root = program / "browser-runtime" / host;
        if (!fs::exists(fs::symlink_status(root))) return {};
        metadata(program, root, host);
        return root / (host == "windows-x64" ? "chrome.exe" : "chrome");
    } catch (...) { invalid(); }
}
void verify_browser_bundle(const fs::path &executable) {
    try {
        const auto host = platform();
        if (host.empty()) invalid();
        const auto absolute = fs::absolute(executable).lexically_normal();
        const auto root = absolute.parent_path();
        const auto program = root.parent_path().parent_path();
        if (bundled_browser(program) != absolute) invalid();
        const auto marker = metadata(program, root, host);
        const auto &files = marker.at("files").as_object();
        std::size_t count = 0;
        for (const auto &entry : fs::recursive_directory_iterator(root)) {
            no_link(entry.path());
            const auto relative = entry.path().lexically_relative(root).generic_string();
            safe_name(relative);
            if (entry.is_directory()) continue;
            if (!entry.is_regular_file()) invalid();
            if (relative == marker_name) continue;
            const auto *expected = files.if_contains(relative);
            if (!expected || file_hash(entry.path()) != expected->as_string()) invalid();
            ++count;
        }
        if (count != files.size()) invalid();
    } catch (...) { invalid(); }
}
}
