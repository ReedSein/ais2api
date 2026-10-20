// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include "ais2api/browser.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <boost/asio.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace ais2api {
namespace {
struct CurlInit {
    CurlInit() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT))
            throw Error(500, "network_init", "Cannot initialize downloader");
    }
    ~CurlInit() { curl_global_cleanup(); }
};
using Curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
Curl curl_handle() {
    static CurlInit init;
    Curl c(curl_easy_init(), curl_easy_cleanup);
    if (!c)
        throw Error(500, "network_init", "Cannot allocate downloader");
    curl_easy_setopt(c.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c.get(), CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_USERAGENT, "ais2api/0.1");
    return c;
}
std::string file_digest(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error(500, "download_read", "Cannot read downloaded archive");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw Error(500, "digest", "Cannot initialize digest");
    std::array<char, 65536> bytes{};
    while (in) {
        in.read(bytes.data(), bytes.size());
        auto n = in.gcount();
        if (n > 0 &&
            EVP_DigestUpdate(context.get(), bytes.data(), static_cast<std::size_t>(n)) != 1)
            throw Error(500, "digest", "Cannot update digest");
    }
    if (!in.eof())
        throw Error(500, "download_read", "Cannot read complete archive");
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned n{};
    if (EVP_DigestFinal_ex(context.get(), digest, &n) != 1)
        throw Error(500, "digest", "Cannot finalize digest");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < n; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 15];
    }
    return out;
}
std::string platform() {
#ifdef _WIN32
    return "windows-x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (std::getenv("PREFIX") &&
        std::string(std::getenv("PREFIX")).find("com.termux") != std::string::npos)
        return "termux-arm64";
    throw Error(400, "unsupported_platform",
                "Automatic setup supports ARM64 Termux, Windows x64 and Ubuntu x64");
#elif defined(__x86_64__)
    return "linux-x64";
#else
    throw Error(400, "unsupported_platform", "Unsupported automatic setup architecture");
#endif
}
json::object manifest() {
    std::vector<fs::path> paths{executable_path().parent_path() / "runtime-manifest.json"};
#ifdef AIS2API_SOURCE_DIR
    paths.emplace_back(fs::path(AIS2API_SOURCE_DIR) / "runtime-manifest.json");
#endif
    for (auto &p : paths)
        if (fs::is_regular_file(p)) {
            auto v = json::parse(read_file(p, 65536));
            if (!v.is_object() || v.as_object().at("version") != 1)
                throw Error(400, "invalid_manifest", "Unsupported runtime manifest");
            return v.as_object();
        }
    throw Error(400, "manifest_missing", "runtime-manifest.json is missing");
}
std::string sh_quote(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\"'\"'";
        else
            out += c;
    }
    return out += "'";
}
void checked_process(const std::vector<std::string> &args) {
    if (run_process(args) != 0)
        throw Error(502, "system_prepare",
                    "System preparation failed; inspect the preceding package-manager output and "
                    "rerun setup");
}
json::object verify_browser(const Config &config, const fs::path &executable) {
    auto verification = config;
    verification.browser_executable = executable;
    verification.headless = true;
    Store store(config.data_dir);
    BrowserGateway gateway(verification, store);
    return gateway.verify_runtime();
}
bool termux_container_exists() {
    auto prefix = std::getenv("TERMUX__PREFIX");
    if (!prefix)
        prefix = std::getenv("PREFIX");
    return prefix && (fs::is_directory(fs::path(prefix) /
                                       "var/lib/proot-distro/containers/ais2api-browser/rootfs") ||
                      fs::is_directory(fs::path(prefix) /
                                       "var/lib/proot-distro/installed-rootfs/ais2api-browser"));
}
bool public_address(const boost::asio::ip::address &address) {
    if (address.is_unspecified() || address.is_loopback() || address.is_multicast())
        return false;
    if (address.is_v4()) {
        auto n = address.to_v4().to_uint();
        auto a = n >> 24, b = (n >> 16) & 255;
        return !(a == 0 || a == 10 || a == 127 || a >= 224 || (a == 169 && b == 254) ||
                 (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
                 (a == 100 && b >= 64 && b <= 127) || (a == 198 && (b == 18 || b == 19)) ||
                 (a == 192 && b == 0));
    }
    auto b = address.to_v6().to_bytes();
    if ((b[0] & 0xfe) == 0xfc || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80))
        return false;
    bool mapped = true;
    for (unsigned i = 0; i < 10; ++i)
        mapped = mapped && b[i] == 0;
    if (mapped && b[10] == 255 && b[11] == 255)
        return public_address(boost::asio::ip::address_v4({b[12], b[13], b[14], b[15]}));
    return b[0] == 0x20 || b[0] == 0x26 || b[0] == 0x28 || b[0] == 0x2a || b[0] == 0x2c;
}
curl_socket_t public_socket(void *, curlsocktype, struct curl_sockaddr *address) {
    bool allowed = false;
    if (address->family == AF_INET) {
        auto *p = reinterpret_cast<sockaddr_in *>(&address->addr);
        allowed = public_address(boost::asio::ip::address_v4(ntohl(p->sin_addr.s_addr)));
    } else if (address->family == AF_INET6) {
        auto *p = reinterpret_cast<sockaddr_in6 *>(&address->addr);
        boost::asio::ip::address_v6::bytes_type b{};
        std::copy_n(reinterpret_cast<const unsigned char *>(&p->sin6_addr), 16, b.begin());
        allowed = public_address(boost::asio::ip::address_v6(b));
    }
    if (!allowed)
        return CURL_SOCKET_BAD;
    return ::socket(address->family, address->socktype, address->protocol);
}
} // namespace
void download_verified(std::string_view url, const fs::path &destination, std::string_view digest,
                       std::string_view proxy, std::uint64_t expected_size) {
    if (!url.starts_with("https://") || digest.size() != 64)
        throw Error(400, "invalid_artifact", "Artifact requires HTTPS and a SHA-256 digest");
    fs::create_directories(destination.parent_path());
    auto part = destination;
    part += ".part";
    if (fs::is_symlink(fs::symlink_status(part)) || fs::is_symlink(fs::symlink_status(destination)))
        throw Error(400, "unsafe_path", "Download path cannot be a symbolic link");
    if (fs::is_regular_file(destination) && file_digest(destination) == digest)
        return;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        auto offset = fs::exists(part) ? fs::file_size(part) : 0;
        const auto cap = expected_size ? expected_size : 1024ull * 1024 * 1024;
        if (offset > cap) {
            fs::remove(part);
            throw Error(413, "download_limit",
                        "Partial download exceeded its permitted archive size");
        }
        if (offset && file_digest(part) == digest) {
            if (fs::exists(destination))
                fs::remove(destination);
            fs::rename(part, destination);
            return;
        }
        std::ofstream out(part, std::ios::binary | std::ios::app);
        if (!out)
            throw Error(500, "download_write", "Cannot write download cache");
        auto c = curl_handle();
        std::string address(url), proxy_value(proxy);
        curl_easy_setopt(c.get(), CURLOPT_URL, address.c_str());
        curl_easy_setopt(c.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c.get(), CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c.get(), CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(c.get(), CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(offset));
        if (!proxy_value.empty())
            curl_easy_setopt(c.get(), CURLOPT_PROXY, proxy_value.c_str());
        struct Download {
            std::ofstream *output;
            std::uint64_t bytes, cap;
            bool exceeded = false;
            Clock::time_point logged = Clock::now();
        } progress{&out, offset, cap};
        curl_easy_setopt(c.get(), CURLOPT_TIMEOUT, 1800L);
        curl_easy_setopt(c.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c.get(), CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(
            c.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                auto &state = *static_cast<Download *>(p);
                if (Clock::now() - state.logged >= std::chrono::seconds(5)) {
                    std::cerr << json::serialize(json::object{{"event", "setup_download"},
                                                              {"received_bytes", state.bytes},
                                                              {"maximum_bytes", state.cap}})
                              << '\n';
                    state.logged = Clock::now();
                }
                return 0;
            });
        curl_easy_setopt(c.get(), CURLOPT_WRITEDATA, &progress);
        curl_easy_setopt(
            c.get(), CURLOPT_WRITEFUNCTION, +[](char *b, size_t s, size_t n, void *p) -> size_t {
                auto size = s * n;
                auto &state = *static_cast<Download *>(p);
                if (size > state.cap - state.bytes) {
                    state.exceeded = true;
                    return 0;
                }
                state.output->write(b, static_cast<std::streamsize>(size));
                if (!*state.output)
                    return 0;
                state.bytes += size;
                return size;
            });
        auto result = curl_easy_perform(c.get());
        long status = 0;
        curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &status);
        out.close();
        if (progress.exceeded) {
            fs::remove(part);
            throw Error(413, "download_limit", "Browser archive exceeded its locked size");
        }
        if (result == CURLE_OK) {
            if (file_digest(part) != digest) {
                fs::remove(part);
                throw Error(502, "digest_mismatch",
                            "Browser archive SHA-256 mismatch; partial archive removed");
            }
            if (fs::exists(destination))
                fs::remove(destination);
            fs::rename(part, destination);
            return;
        }
        if (result == CURLE_RANGE_ERROR || status == 416) {
            fs::remove(part);
            continue;
        }
        if (attempt == 2)
            throw Error(502, "download_failed",
                        std::string("Archive download failed: ") + curl_easy_strerror(result) +
                            "; partial bytes retained for a later retry");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw Error(502, "download_failed", "Archive server did not accept resumed downloads");
}
void extract_archive(const fs::path &input, const fs::path &destination) {
    if (fs::exists(destination) && (!fs::is_directory(destination) || !fs::is_empty(destination)))
        throw Error(409, "directory_not_empty",
                    "Extraction destination must be an empty owned staging directory");
    secure_directory(destination);
    auto root = fs::weakly_canonical(destination);
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(),
                                                                  archive_read_free);
    archive_read_support_filter_gzip(reader.get());
    archive_read_support_format_tar(reader.get());
    archive_read_support_format_zip(reader.get());
#ifdef _WIN32
    auto result = archive_read_open_filename_w(reader.get(), input.c_str(), 65536);
#else
    auto result = archive_read_open_filename(reader.get(), input.c_str(), 65536);
#endif
    if (result != ARCHIVE_OK)
        throw Error(400, "invalid_archive", "Cannot open runtime archive");
    archive_entry *entry{};
    std::set<fs::path> seen;
    std::uint64_t total = 0;
    unsigned count = 0;
    while ((result = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (++count > 100000)
            throw Error(413, "archive_limit", "Archive contains too many entries");
        auto pathname = archive_entry_pathname_utf8(entry);
        if (!pathname)
            throw Error(400, "archive_path", "Archive path must be UTF-8");
        std::string name(pathname);
        std::replace(name.begin(), name.end(), '\\', '/');
        auto relative = fs::u8path(name);
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
            name.find(':') != std::string::npos || archive_entry_symlink(entry) ||
            archive_entry_hardlink(entry))
            throw Error(400, "archive_path", "Unsafe archive entry");
        for (auto &piece : relative) {
            if (piece == "..")
                throw Error(400, "archive_path", "Archive entry escapes staging directory");
#ifdef _WIN32
            auto component = piece.string();
            if (component != "." && !component.empty()) {
                auto base = component.substr(0, component.find('.'));
                std::transform(base.begin(), base.end(), base.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                bool device =
                    base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
                    (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
                     base[3] >= '1' && base[3] <= '9');
                if (device || component.back() == '.' || component.back() == ' ' ||
                    component.find_first_of("<>\"|?*") != std::string::npos)
                    throw Error(400, "archive_path", "Ambiguous Windows archive path");
            }
#endif
        }
        auto target = (root / relative).lexically_normal();
        if (!seen.insert(target).second && archive_entry_filetype(entry) != AE_IFDIR)
            throw Error(400, "archive_path", "Duplicate archive file");
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            fs::create_directories(target);
            continue;
        }
        if (archive_entry_filetype(entry) != AE_IFREG)
            throw Error(400, "archive_path", "Special archive entries are not supported");
        // Filesystem aliases (Windows case folding / short names) may name an
        // already extracted entry even when the lexical paths differ.
        if (fs::exists(target))
            throw Error(400, "archive_path", "Archive file aliases an existing entry");
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        if (!out)
            throw Error(500, "extract_write", "Cannot create extracted file");
        std::array<char, 65536> block{};
        la_ssize_t n = 0;
        while ((n = archive_read_data(reader.get(), block.data(), block.size())) > 0) {
            total += static_cast<std::uint64_t>(n);
            if (total > 4ull * 1024 * 1024 * 1024)
                throw Error(413, "archive_limit", "Expanded archive exceeds four GiB");
            out.write(block.data(), n);
            if (!out)
                throw Error(500, "extract_write", "Cannot write extracted file");
        }
        if (n < 0)
            throw Error(400, "invalid_archive", "Truncated runtime archive");
        out.close();
#ifndef _WIN32
        fs::permissions(target, static_cast<fs::perms>(archive_entry_perm(entry) & 0777));
#endif
    }
    if (result != ARCHIVE_EOF)
        throw Error(400, "invalid_archive", "Truncated runtime archive");
}
json::object setup_runtime(Config &config, bool approved) {
    auto m = manifest();
    auto system = platform();
    auto version = string_or(m, "browser_version");
    auto artifact = m.at("artifacts").as_object().at(system).as_object();
    auto root = config.data_dir / "runtime";
    secure_directory(config.data_dir);
    secure_directory(root);
    auto journal = root / "setup.json";
    auto stage = [&](std::string name) {
        std::cerr << json::serialize(json::object{{"event", "setup_stage"},
                                                  {"stage", name},
                                                  {"browser_version", version},
                                                  {"platform", system}})
                  << '\n';
        atomic_write(journal,
                     json::serialize(json::object{{"version", 1},
                                                  {"stage", name},
                                                  {"browser_version", version},
                                                  {"platform", system}}),
                     true);
    };
    stage("system-check");
    if (system != "windows-x64" && !approved)
        throw Error(409, "system_confirmation_required",
                    "System packages may be installed. Review the setup documentation and run "
                    "setup --yes to approve package-manager changes");
    if (system == "linux-x64") {
        auto os = read_file("/etc/os-release", 65536);
        if (os.find("ID=ubuntu") == std::string::npos || os.find("24.04") == std::string::npos)
            throw Error(400, "unsupported_distribution",
                        "Automatic Linux setup supports Ubuntu 24.04 x64; manually configure "
                        "browser_executable on other distributions");
        std::vector<std::string> prefix;
#ifndef _WIN32
        if (geteuid() != 0)
            prefix = {"sudo", "--"};
#endif
        auto command = prefix;
        command.insert(command.end(), {"apt-get", "update"});
        checked_process(command);
        command = prefix;
        command.insert(command.end(), {"apt-get",
                                       "install",
                                       "-y",
                                       "libnss3",
                                       "libnspr4",
                                       "libatk1.0-0t64",
                                       "libatk-bridge2.0-0t64",
                                       "libcups2t64",
                                       "libdrm2",
                                       "libxkbcommon0",
                                       "libxcomposite1",
                                       "libxdamage1",
                                       "libxfixes3",
                                       "libxrandr2",
                                       "libgbm1",
                                       "libasound2t64",
                                       "libgtk-3-0t64",
                                       "fonts-noto-cjk",
                                       "fonts-noto-color-emoji",
                                       "ca-certificates"});
        checked_process(command);
    } else if (system == "termux-arm64") {
        checked_process({"pkg", "install", "-y", "proot-distro"});
        auto created = root / "proot-owned.json";
        if (!termux_container_exists()) {
            atomic_write(created, "{\"version\":1,\"phase\":\"creating\"}", true);
            checked_process(
                {"proot-distro", "install", "ubuntu:24.04", "--name", "ais2api-browser"});
        } else if (!fs::exists(created))
            throw Error(409, "unknown_container",
                        "Existing ais2api-browser container has no ownership marker");
        checked_process({"proot-distro", "login", "ais2api-browser", "--", "apt-get", "update"});
        checked_process({"proot-distro",
                         "login",
                         "ais2api-browser",
                         "--",
                         "apt-get",
                         "install",
                         "-y",
                         "libnss3",
                         "libnspr4",
                         "libatk1.0-0t64",
                         "libatk-bridge2.0-0t64",
                         "libcups2t64",
                         "libdrm2",
                         "libxkbcommon0",
                         "libxcomposite1",
                         "libxdamage1",
                         "libxfixes3",
                         "libxrandr2",
                         "libgbm1",
                         "libasound2t64",
                         "libgtk-3-0t64",
                         "fonts-noto-cjk",
                         "fonts-noto-color-emoji",
                         "ca-certificates"});
        atomic_write(created, "{\"version\":1,\"phase\":\"ready\"}", true);
    }
    auto target = root / ("chromium-" + version + "-" + system);
    auto executable = target / (system == "windows-x64" ? "chrome.exe" : "chrome");
    if (!fs::is_regular_file(executable)) {
        if (fs::exists(target))
            throw Error(409, "incomplete_runtime",
                        "An incomplete final runtime directory exists; preserve it and rerun with "
                        "a clean version directory");
        if (fs::space(root).available < 2ull * 1024 * 1024 * 1024)
            throw Error(507, "disk_space",
                        "At least two GiB of free space is required for browser staging");
        stage("download");
        auto cache =
            root / "downloads" / (system == "windows-x64" ? "browser.zip" : "browser.tar.gz");
        download_verified(string_or(artifact, "url"), cache, string_or(artifact, "sha256"),
                          config.proxy, json::value_to<std::uint64_t>(artifact.at("size")));
        stage("extract");
        auto staging = root / ("staging-" + random_hex(8));
        try {
            extract_archive(cache, staging);
            auto binary = staging / executable.filename();
            if (!fs::is_regular_file(binary))
                throw Error(502, "archive_layout",
                            "Runtime archive does not contain the expected executable");
            atomic_write(staging / ".ais2api-runtime.json",
                         json::serialize(json::object{{"browser_version", version},
                                                      {"sha256", string_or(artifact, "sha256")}}),
                         true);
            fs::rename(staging, target);
        } catch (...) {
            std::error_code ec;
            fs::remove_all(staging, ec);
            throw;
        }
    }
    auto ownership = target / ".ais2api-runtime.json";
    if (!fs::is_regular_file(ownership))
        throw Error(409, "unknown_runtime",
                    "Existing browser directory has no project ownership marker");
    auto owner = json::parse(read_file(ownership, 65536)).as_object();
    if (string_or(owner, "browser_version") != version ||
        string_or(owner, "sha256") != string_or(artifact, "sha256"))
        throw Error(409, "unknown_runtime",
                    "Existing browser directory does not match the locked artifact");
    fs::path launcher = executable;
    if (system == "termux-arm64") {
        auto prefix = std::getenv("PREFIX");
        if (!prefix)
            throw Error(400, "environment", "Missing Termux PREFIX");
        launcher = target / "run-browser.sh";
        auto target_string = fs::absolute(target).string(),
             data = fs::absolute(config.data_dir).string();
        auto text = "#!" + std::string(prefix) + "/bin/sh\nexec proot-distro login --bind " +
                    sh_quote(target_string + ":" + target_string) + " --bind " +
                    sh_quote(data + ":" + data) + " ais2api-browser -- " +
                    sh_quote(fs::absolute(executable).string()) + " \"$@\"\n";
        atomic_write(launcher, text, true);
#ifndef _WIN32
        fs::permissions(launcher, fs::perms::owner_all);
#endif
    }
    stage("verify");
    auto browser_verification = verify_browser(config, launcher);
    if (!config.browser_executable.empty() && config.browser_executable != launcher)
        atomic_write(root / "previous.json",
                     json::serialize(json::object{
                         {"browser_executable", path_utf8(config.browser_executable)}}),
                     true);
    auto previous = config.browser_executable;
    config.browser_executable = launcher;
    try {
        atomic_write(config.config_file, json::serialize(config.to_json()), true);
    } catch (...) {
        config.browser_executable = previous;
        throw;
    }
    stage("complete");
    return {{"status", "prepared"},
            {"browser_version", version},
            {"browser_executable", path_utf8(launcher)},
            {"verified", "local_cdp_and_isolated_context"},
            {"browser_verification", browser_verification},
            {"note", "Run accounts probe and an actual generation to verify Google access"}};
}
json::object rollback_runtime(Config &config) {
    auto file = config.data_dir / "runtime/previous.json";
    auto previous = json::parse(read_file(file, 65536)).as_object();
    auto path = fs::u8path(string_or(previous, "browser_executable"));
    if (!fs::is_regular_file(path))
        throw Error(409, "rollback_missing", "Previous browser executable no longer exists");
    auto browser_verification = verify_browser(config, path);
    auto current = config.browser_executable;
    config.browser_executable = path;
    try {
        atomic_write(config.config_file, json::serialize(config.to_json()), true);
    } catch (...) {
        config.browser_executable = current;
        throw;
    }
    atomic_write(file, json::serialize(json::object{{"browser_executable", path_utf8(current)}}),
                 true);
    return {{"status", "rolled_back"},
            {"browser_executable", path_utf8(path)},
            {"browser_verification", browser_verification}};
}
json::object doctor(const Config &config) {
    return {{"config_version", config.version},
            {"browser_configured", !config.browser_executable.empty()},
            {"browser_exists", fs::is_regular_file(config.browser_executable)},
            {"data_directory", path_utf8(config.data_dir)},
            {"manifest_available", !manifest().empty()},
            {"google_access", "not_checked"}};
}
std::string fetch_public_media(std::string_view input, std::size_t limit, std::string_view proxy,
                               std::stop_token cancel) {
    std::string url(input), proxy_text(proxy);
    for (unsigned redirect = 0; redirect < 4; ++redirect) {
        check_cancel(cancel);
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
        if (curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0))
            throw Error(400, "media_url", "Invalid image URL");
        auto part = [&](CURLUPart p) {
            char *text = nullptr;
            auto result = curl_url_get(parsed.get(), p, &text, 0);
            std::string value = result == CURLUE_OK ? text : "";
            curl_free(text);
            return value;
        };
        if (part(CURLUPART_SCHEME) != "https" || !part(CURLUPART_USER).empty() ||
            !part(CURLUPART_PASSWORD).empty() ||
            (!part(CURLUPART_PORT).empty() && part(CURLUPART_PORT) != "443"))
            throw Error(400, "media_url",
                        "Only public HTTPS images without URL credentials are supported");
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver(io);
        boost::system::error_code ec;
        auto addresses = resolver.resolve(part(CURLUPART_HOST), "443", ec);
        if (ec || addresses.empty())
            throw Error(400, "media_url", "Cannot resolve image host");
        for (auto &a : addresses)
            if (!public_address(a.endpoint().address()))
                throw Error(400, "media_url", "Private image destinations are not allowed");
        struct Buffer {
            std::string bytes;
            std::size_t max;
            bool exceeded = false;
        } data{{}, limit};
        auto c = curl_handle();
        curl_easy_setopt(c.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(c.get(), CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(
            c.get(), CURLOPT_WRITEFUNCTION, +[](char *b, size_t s, size_t n, void *p) -> size_t {
                auto &d = *static_cast<Buffer *>(p);
                auto length = s * n;
                if (length > d.max - d.bytes.size()) {
                    d.exceeded = true;
                    return 0;
                }
                d.bytes.append(b, length);
                return length;
            });
        curl_easy_setopt(c.get(), CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(c.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c.get(), CURLOPT_XFERINFODATA, &cancel);
        curl_easy_setopt(
            c.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return static_cast<std::stop_token *>(p)->stop_requested() ? 1 : 0;
            });
        if (proxy_text.empty())
            curl_easy_setopt(c.get(), CURLOPT_OPENSOCKETFUNCTION, public_socket);
        else
            curl_easy_setopt(c.get(), CURLOPT_PROXY, proxy_text.c_str());
        auto result = curl_easy_perform(c.get());
        check_cancel(cancel);
        if (data.exceeded)
            throw Error(413, "media_limit", "Remote image exceeds size limit");
        if (result != CURLE_OK)
            throw Error(502, "media_fetch", "Unable to retrieve remote image");
        long status = 0;
        curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &status);
        if (status >= 300 && status < 400) {
            char *target = nullptr;
            curl_easy_getinfo(c.get(), CURLINFO_REDIRECT_URL, &target);
            if (!target)
                throw Error(502, "media_fetch", "Image redirect has no destination");
            url = target;
            continue;
        }
        if (status != 200)
            throw Error(502, "media_fetch", "Image server returned an error");
        char *type = nullptr;
        curl_easy_getinfo(c.get(), CURLINFO_CONTENT_TYPE, &type);
        std::string mime = type ? type : "";
        mime = mime.substr(0, mime.find(';'));
        if (!mime.starts_with("image/") || mime.size() > 64)
            throw Error(400, "media_type", "Remote resource is not an image");
        std::string b64(4 * ((data.bytes.size() + 2) / 3), '\0');
        auto n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(b64.data()),
                                 reinterpret_cast<const unsigned char *>(data.bytes.data()),
                                 static_cast<int>(data.bytes.size()));
        b64.resize(static_cast<std::size_t>(n));
        return "data:" + mime + ";base64," + b64;
    }
    throw Error(400, "media_redirect", "Too many image redirects");
}
} // namespace ais2api
