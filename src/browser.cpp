// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/browser.hpp"
#include "../browser/cdp.hpp"
#include "../browser/rpc_error.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cctype>
#include <cwchar>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
extern char **environ;
#endif

namespace ais2api {
browser_detail::IdentityVerdict browser_detail::identity_verdict(const json::object& identity, std::string_view expected_email) {
    if (bool_or(identity, "identityAmbiguous")) return IdentityVerdict::ambiguous;
    auto normalized = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    const auto email = normalized(string_or(identity, "email"));
    if (!bool_or(identity, "identityReady") || email.empty()) return IdentityVerdict::pending;
    const auto expected = normalized(std::string(expected_email));
    return expected.empty() || expected == email ? IdentityVerdict::verified : IdentityVerdict::mismatch;
}
json::array browser_detail::import_cookies(const json::array& cookies, std::size_t* skipped_partition_cookies) {
    json::array result;
    if (skipped_partition_cookies) *skipped_partition_cookies = 0;
    for (const auto& value : cookies) {
        if (!value.is_object()) continue;
        const auto& source = value.as_object();
        if (source.contains("partitionKey") || bool_or(source, "partitionKeyOpaque")) {
            if (skipped_partition_cookies) ++*skipped_partition_cookies;
            continue;
        }
        auto domain = string_or(source, "domain");
        std::transform(domain.begin(), domain.end(), domain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        if (domain != "google.com" && !domain.ends_with(".google.com")) continue;
        const auto name = source.if_contains("name"), content = source.if_contains("value");
        if (!name || !name->is_string() || name->as_string().empty() || !content || !content->is_string()) continue;
        json::object cookie{{"name", *name}, {"value", *content}, {"domain", source.at("domain")}};
        if (auto p = source.if_contains("path"); p && p->is_string()) cookie["path"] = *p;
        for (auto key : {"secure", "httpOnly"}) if (auto p = source.if_contains(key); p && p->is_bool()) cookie[key] = *p;
        if (auto p = source.if_contains("sameSite"); p && p->is_string() && (p->as_string() == "Strict" || p->as_string() == "Lax" || p->as_string() == "None")) cookie["sameSite"] = *p;
        if (auto p = source.if_contains("expires"); p && p->is_number() && json::value_to<double>(*p) > 0) cookie["expires"] = *p;
        result.push_back(std::move(cookie));
    }
    return result;
}
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

[[noreturn]] void incompatible(std::string message) { throw Error(502, "browser_incompatible", std::move(message)); }
std::string quote_js(std::string_view s) { return json::serialize(json::value(s)); }
std::string decode64(std::string_view encoded) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (encoded.size() > 4 * ((32768 + 2) / 3) || encoded.size() % 4) incompatible("Invalid browser relay chunk");
    std::string bytes;
    bytes.reserve(encoded.size() * 3 / 4);
    unsigned bits = 0, count = 0;
    bool padding = false;
    for (char c : encoded) {
        if (c == '=') { padding = true; continue; }
        const auto n = alphabet.find(c);
        if (padding || n == std::string_view::npos) incompatible("Invalid base64 browser chunk");
        bits = (bits << 6) | static_cast<unsigned>(n); count += 6;
        if (count >= 8) { count -= 8; bytes.push_back(static_cast<char>((bits >> count) & 255)); }
    }
    if (bytes.size() > 32768) incompatible("Browser exceeded byte credit");
    return bytes;
}
fs::path executable_directory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size && size < buffer.size()) { buffer.resize(size); return fs::path(buffer).parent_path(); }
#else
    std::array<char, 4096> buffer{};
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size > 0) return fs::path(std::string(buffer.data(), static_cast<std::size_t>(size))).parent_path();
#endif
    return fs::current_path();
}
std::string bridge_source() {
    std::vector<fs::path> candidates{executable_directory() / "browser" / "bridge.js"};
#ifdef AIS2API_ASSET_DIR
    candidates.emplace_back(fs::path(AIS2API_ASSET_DIR) / "bridge.js");
    candidates.emplace_back(fs::path(AIS2API_ASSET_DIR) / "browser" / "bridge.js");
#endif
    for (const auto& path : candidates) if (fs::is_regular_file(path)) return read_file(path, 256 * 1024);
    incompatible("Cannot locate browser/bridge.js beside the executable or in AIS2API_ASSET_DIR");
}

class OwnedProcess {
public:
    ~OwnedProcess() { stop(); }
    void start(const fs::path& executable, const std::vector<std::string>& args, const fs::path& private_root) {
        stop();
        // Build a child-only environment before fork/CreateProcess. Never mutate
        // this multithreaded service's process-wide environment.
        const auto home = fs::absolute(private_root / "home");
        const auto cache = fs::absolute(private_root / "cache");
        const auto settings = fs::absolute(private_root / "settings");
        const auto temporary = fs::absolute(private_root / "tmp");
        const auto data = fs::absolute(private_root / "files");
        const auto state = fs::absolute(private_root / "state");
        for (const auto& directory : {home, cache, settings, temporary, data, state}) secure_directory(directory);
        const auto prefix = std::getenv("PREFIX");
        const bool termux = std::getenv("TERMUX_VERSION") || (prefix && std::string_view(prefix).find("com.termux") != std::string_view::npos);
#ifdef _WIN32
        auto wide = [](const std::string& s) {
            if (s.empty()) return std::wstring{};
            int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (!n) throw Error(500, "browser_launch", "Invalid UTF-8 launch argument");
            std::wstring result(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
            return result;
        };
        auto quote = [](const std::wstring& value) {
            std::wstring result = L"\"";
            unsigned slashes = 0;
            for (wchar_t c : value) {
                if (c == L'\\') { ++slashes; continue; }
                if (c == L'"') { result.append(slashes * 2 + 1, L'\\'); result += c; }
                else { result.append(slashes, L'\\'); result += c; }
                slashes = 0;
            }
            result.append(slashes * 2, L'\\'); result += L'"'; return result;
        };
        std::wstring command = quote(executable.wstring());
        for (const auto& arg : args) command += L" " + quote(wide(arg));
        auto environment_less = [](const std::wstring& a, const std::wstring& b) {
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        };
        std::map<std::wstring, std::wstring, decltype(environment_less)> inherited(environment_less);
        auto raw_environment = GetEnvironmentStringsW();
        if (!raw_environment) throw Error(500, "browser_launch", "Cannot read browser environment");
        for (auto entry = raw_environment; *entry; entry += std::wcslen(entry) + 1) {
            std::wstring value(entry);
            // Hidden drive-current-directory variables begin with '=' (e.g. =C:).
            const auto separator = value.find(L'=', value.front() == L'=' ? 1 : 0);
            if (separator != std::wstring::npos) inherited[value.substr(0, separator)] = value.substr(separator + 1);
        }
        FreeEnvironmentStringsW(raw_environment);
        inherited[L"LOCALAPPDATA"] = cache.wstring();
        inherited[L"APPDATA"] = settings.wstring();
        inherited[L"TEMP"] = temporary.wstring();
        inherited[L"TMP"] = temporary.wstring();
        if (!termux) {
            inherited[L"USERPROFILE"] = home.wstring();
            inherited[L"HOME"] = home.wstring();
            inherited[L"HOMEDRIVE"] = home.root_name().wstring();
            inherited[L"HOMEPATH"] = home.relative_path().empty() ? L"\\" : L"\\" + home.relative_path().wstring();
        }
        std::vector<wchar_t> environment;
        for (const auto& [key, value] : inherited) {
            environment.insert(environment.end(), key.begin(), key.end()); environment.push_back(L'=');
            environment.insert(environment.end(), value.begin(), value.end()); environment.push_back(L'\0');
        }
        environment.push_back(L'\0');
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) throw Error(500, "browser_launch", "CreateJobObject failed");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { stop(); throw Error(500, "browser_launch", "Cannot configure browser Job Object"); }
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &pi)) { stop(); throw Error(500, "browser_launch", "CreateProcessW failed"); }
        process_ = pi.hProcess;
        if (!AssignProcessToJobObject(job_, process_)) { TerminateProcess(process_, 1); CloseHandle(pi.hThread); stop(); throw Error(500, "browser_launch", "Cannot assign browser to Job Object"); }
        if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) { CloseHandle(pi.hThread); stop(); throw Error(500, "browser_launch", "Cannot resume browser process"); }
        CloseHandle(pi.hThread);
#else
        std::vector<std::string> values{executable.string()};
        values.insert(values.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& value : values) argv.push_back(value.data());
        argv.push_back(nullptr);
        std::map<std::string, std::string> inherited;
        for (auto entry = ::environ; entry && *entry; ++entry) {
            std::string value(*entry);
            auto separator = value.find('=');
            if (separator != std::string::npos) inherited[value.substr(0, separator)] = value.substr(separator + 1);
        }
        inherited["XDG_CONFIG_HOME"] = settings.string();
        inherited["XDG_CACHE_HOME"] = cache.string();
        inherited["XDG_DATA_HOME"] = data.string();
        inherited["XDG_STATE_HOME"] = state.string();
        inherited["TMPDIR"] = temporary.string();
#if defined(__linux__) && !defined(__ANDROID__)
        if (!termux) {
            // Chromium binds AF_UNIX singleton sockets below TMPDIR. Their
            // 108-byte address limit otherwise breaks portable installs in
            // deeply nested directories. Keep the actual files in this private
            // profile; a held parent directory fd provides a short kernel alias.
            temporary_directory_fd_ = open(temporary.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (temporary_directory_fd_ < 0) throw Error(500, "browser_launch", "Cannot open private browser temporary directory");
            const auto alias = "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(temporary_directory_fd_);
            if (!fs::is_directory(alias)) throw Error(500, "browser_launch", "Cannot access private browser temporary directory through /proc");
            inherited["TMPDIR"] = alias;
        }
#endif
        if (!termux) inherited["HOME"] = home.string();
        std::vector<std::string> environment;
        for (const auto& [key, value] : inherited) environment.push_back(key + "=" + value);
        std::vector<char*> envp;
        for (auto& value : environment) envp.push_back(value.data());
        envp.push_back(nullptr);
        pid_ = fork();
        if (pid_ < 0) throw Error(500, "browser_launch", "fork failed");
        if (pid_ == 0) {
            if (setsid() < 0) _exit(126);
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); dup2(nullfd, STDOUT_FILENO); dup2(nullfd, STDERR_FILENO); if (nullfd > 2) ::close(nullfd); }
            execve(argv[0], argv.data(), envp.data()); _exit(127);
        }
#endif
    }
    bool alive() {
#ifdef _WIN32
        return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
        if (pid_ <= 0) return false;
        siginfo_t info{};
        // Leave an exited leader unreaped until stop(), preventing PID/group
        // reuse while descendants are still owned by this gateway.
        return waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0;
#endif
    }
    void stop() noexcept {
#ifdef _WIN32
        if (job_) { CloseHandle(job_); job_ = nullptr; }
        if (process_) { WaitForSingleObject(process_, 5000); CloseHandle(process_); process_ = nullptr; }
#else
        if (pid_ > 0) {
            kill(-pid_, SIGTERM);
            kill(pid_, SIGTERM);
            for (unsigned i = 0; i < 30; ++i) { siginfo_t info{}; if (waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) != 0 || info.si_pid) break; std::this_thread::sleep_for(20ms); }
            kill(-pid_, SIGKILL);
            int status = 0; while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
        }
        if (temporary_directory_fd_ >= 0) { ::close(temporary_directory_fd_); temporary_directory_fd_ = -1; }
#endif
    }
private:
#ifdef _WIN32
    HANDLE process_ = nullptr, job_ = nullptr;
#else
    pid_t pid_ = -1;
    int temporary_directory_fd_ = -1;
#endif
};

using Cdp = browser_detail::Cdp;

// The DevToolsActivePort file belongs to our fresh private profile. No global
// process or existing listening browser is adopted or killed.
std::pair<unsigned short, std::string> read_endpoint(const fs::path& profile) {
    std::ifstream input(profile / "DevToolsActivePort");
    std::string port, target;
    if (!std::getline(input, port) || !std::getline(input, target)) return {};
    if (port.empty() || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); })) return {};
    try {
        const auto number = std::stoul(port);
        if (!number || number > 65535 || !target.starts_with("/devtools/browser/")) return {};
        return {static_cast<unsigned short>(number), target};
    } catch (...) { return {}; }
}
void pause(std::stop_token token, std::chrono::milliseconds duration) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) { check_cancel(token); std::this_thread::sleep_for(20ms); }
}
std::string content_hash(const json::array& wire) {
    std::string joined;
    bool first = true;
    if (wire.size() > 1 && wire[1].is_array()) for (const auto& content : wire[1].as_array()) {
        if (!content.is_array() || content.as_array().empty() || !content.as_array()[0].is_array()) continue;
        for (const auto& part : content.as_array()[0].as_array()) {
            if (!first) joined += ' ';
            first = false;
            if (!part.is_array()) continue;
            const auto& p = part.as_array();
            if (p.size() > 1 && p[1].is_string()) joined += p[1].as_string();
            else if (p.size() > 2 && p[2].is_array()) {
                const auto& data = p[2].as_array();
                if (data.size() > 1 && data[1].is_string()) joined += data[1].as_string();
            }
            // Function calls/results and executable code/results contribute an
            // empty field, preserving the page's single-space join semantics.
        }
    }
    return sha256(joined);
}
} // namespace

std::string browser_detail::wire_content_hash(const json::array& wire) { return content_hash(wire); }

struct BrowserGateway::Impl {
    Config config;
    Store& store;
    std::mutex mutex;
    OwnedProcess process;
    std::unique_ptr<Cdp> cdp;
    fs::path profile;
    std::string context, session, account_id, account_fingerprint;
    json::array base_template;
    json::array cached_models;
    Clock::time_point models_at{};
    std::string last_status = "not_started";
    explicit Impl(const Config& c, Store& s) : config(c), store(s) {}
    ~Impl() { close(); }
    void close() noexcept {
        if (cdp) cdp->close();
        cdp.reset(); process.stop(); context.clear(); session.clear(); account_id.clear(); account_fingerprint.clear(); base_template.clear(); cached_models.clear();
        if (!profile.empty()) { std::error_code ec; fs::remove_all(profile, ec); profile.clear(); }
        last_status = "closed";
    }
    json::value eval(std::string expression, std::stop_token token, std::chrono::seconds timeout = 30s) { return cdp->evaluate(std::move(expression), session, token, timeout); }
    void ensure_browser(std::stop_token token) {
        check_cancel(token);
        if (cdp && beast::get_lowest_layer(cdp->socket).socket().is_open() && process.alive()) return;
        close();
        if (config.browser_executable.empty() || !fs::is_regular_file(config.browser_executable)) throw Error(503, "browser_not_configured", "Configure an executable browser or Termux launcher path");
        profile = config.data_dir / "runtime" / ("browser-" + random_hex(12));
        secure_directory(profile);
        std::vector<std::string> args{"--remote-debugging-address=127.0.0.1", "--remote-debugging-port=0", "--user-data-dir=" + path_utf8(fs::absolute(profile)), "--no-first-run", "--no-default-browser-check", "--disable-background-networking", "--disable-dev-shm-usage", "--disable-gpu", "--renderer-process-limit=1"};
        for (const auto& directory : {profile / "crash-dumps", profile / "disk-cache", profile / "logs"}) secure_directory(directory);
        args.push_back("--crash-dumps-dir=" + path_utf8(fs::absolute(profile / "crash-dumps")));
        args.push_back("--disk-cache-dir=" + path_utf8(fs::absolute(profile / "disk-cache")));
        args.push_back("--log-file=" + path_utf8(fs::absolute(profile / "logs" / "browser.log")));
        if (config.headless) args.push_back("--headless=new");
#if !defined(_WIN32) && !defined(__APPLE__)
        // proot's root user and Android cannot provide the Chromium sandbox.
        if (std::getenv("TERMUX_VERSION") || geteuid() == 0) args.push_back("--no-sandbox");
#endif
        if (!config.proxy.empty()) args.push_back("--proxy-server=" + config.proxy);
        args.push_back("about:blank");
        try {
            process.start(config.browser_executable, args, profile);
            const auto deadline = Clock::now() + 30s;
            while (Clock::now() < deadline) {
                check_cancel(token);
                if (!process.alive()) throw Error(502, "browser_launch", "Browser exited before DevTools became ready");
                auto endpoint = read_endpoint(profile);
                if (endpoint.first) { cdp = std::make_unique<Cdp>(); cdp->connect(endpoint.first, endpoint.second, token); last_status = "browser_ready"; return; }
                pause(token, 100ms);
            }
            throw Error(504, "browser_timeout", "Browser did not publish a DevTools endpoint");
        } catch (...) { close(); throw; }
    }
    void dispose_context(std::stop_token token) {
        if (!context.empty()) cdp->call("Target.disposeBrowserContext", {{"browserContextId", context}}, {}, token);
        context.clear(); session.clear(); account_id.clear(); account_fingerprint.clear(); base_template.clear(); cached_models.clear();
    }
    json::object identity(std::stop_token token, bool allow_login = false, std::chrono::seconds timeout = 30s) {
        auto value = eval("globalThis.__ais2bridge.identity()", token, timeout);
        if (!value.is_object()) incompatible("Page identity probe unavailable");
        auto object = value.as_object();
        if (bool_or(object, "login") && !allow_login) throw Error(401, "session_expired", "Google redirected the account to sign-in");
        if (bool_or(object, "regionBlocked")) throw Error(403, "region_blocked", "AI Studio is unavailable in the browser region");
        return object;
    }
    void await_account_identity(const Account& account, std::stop_token token, bool require_editor) {
        const auto deadline = Clock::now() + 60s;
        std::optional<Clock::time_point> login_since, mismatch_since;
        auto last = browser_detail::IdentityVerdict::pending;
        while (Clock::now() < deadline) {
            check_cancel(token);
            try {
                auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
                if (remaining < 1s) break;
                auto exists = eval("!!globalThis.__ais2bridge", token, std::min(5s, remaining));
                if (exists.is_bool() && exists.as_bool()) {
                    remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
                    if (remaining < 1s) break;
                    const auto info = identity(token, true, std::min(5s, remaining));
                    if (bool_or(info, "login")) {
                        mismatch_since.reset();
                        if (!login_since) login_since = Clock::now();
                        if (Clock::now() - *login_since >= 10s)
                            throw Error(401, "session_expired", "Google remained on sign-in after the navigation grace period");
                    } else {
                        login_since.reset();
                        last = browser_detail::identity_verdict(info, account.email);
                        if (last == browser_detail::IdentityVerdict::verified && (!require_editor || bool_or(info, "ready"))) return;
                        if (last == browser_detail::IdentityVerdict::mismatch) {
                            if (!mismatch_since) mismatch_since = Clock::now();
                            if (Clock::now() - *mismatch_since >= 2s)
                                throw Error(401, "identity_mismatch", "The verified Google account differs from the selected account");
                        } else mismatch_since.reset();
                    }
                } else { login_since.reset(); mismatch_since.reset(); last = browser_detail::IdentityVerdict::pending; }
            } catch (const Error& error) {
                // Redirects can replace an execution context while the account
                // badge is loading. Missing identity is never a proven mismatch.
                if (error.code != "browser_incompatible") throw;
                login_since.reset(); mismatch_since.reset(); last = browser_detail::IdentityVerdict::pending;
            }
            pause(token, 250ms);
        }
        if (last == browser_detail::IdentityVerdict::verified)
            incompatible("AI Studio page did not expose a supported ready prompt editor");
        throw Error(409, "identity_unverified", last == browser_detail::IdentityVerdict::ambiguous
            ? "AI Studio exposed multiple account identities" : "AI Studio account identity did not finish loading");
    }
    void ensure_account(const Account& account, std::stop_token token, bool bootstrap, bool identity_only = false) {
        ensure_browser(token);
        auto fingerprint = sha256(json::serialize(account.storage_state) + "\n" + account.auth_user + "\n" + account.email);
        bool verified_session = false;
        if (account.id != account_id || fingerprint != account_fingerprint || session.empty()) {
            // Dispose before create: never keep two account contexts resident.
            dispose_context(token);
            try {
                context = string_or(cdp->call("Target.createBrowserContext", {{"disposeOnDetach", true}}, {}, token), "browserContextId");
                if (context.empty()) incompatible("Browser did not create an isolated context");
                auto cookies = account.storage_state.if_contains("cookies");
                if (!cookies || !cookies->is_array() || cookies->as_array().empty()) throw Error(401, "missing_cookies", "Account has no storage-state cookies");
                json::array sanitized;
                for (const auto& value : cookies->as_array()) {
                    if (!value.is_object()) continue;
                    const auto& original = value.as_object();
                    json::object cookie;
                    for (auto key : {"name", "value", "domain", "path", "secure", "httpOnly", "sameSite", "partitionKey"}) if (auto p = original.if_contains(key)) cookie[key] = *p;
                    if (auto expires = original.if_contains("expires"); expires && expires->is_number() && json::value_to<double>(*expires) > 0) cookie["expires"] = *expires;
                    sanitized.push_back(std::move(cookie));
                }
                cdp->call("Storage.setCookies", {{"cookies", std::move(sanitized)}, {"browserContextId", context}}, {}, token);
                auto target = string_or(cdp->call("Target.createTarget", {{"url", "about:blank"}, {"browserContextId", context}}, {}, token), "targetId");
                if (target.empty()) incompatible("Browser did not create an isolated page");
                session = string_or(cdp->call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}}, {}, token), "sessionId");
                if (session.empty()) incompatible("Browser did not attach the page session");
                cdp->call("Page.enable", {}, session, token);
                cdp->call("Runtime.enable", {}, session, token);
                cdp->call("Page.addScriptToEvaluateOnNewDocument", {{"source", bridge_source()}}, session, token);
                if (auto origins = account.storage_state.if_contains("origins"); origins && origins->is_array()) {
                    const auto restore = "(() => {const entries=" + json::serialize(*origins) + ";for(const e of entries){if(e.origin!==location.origin||!Array.isArray(e.localStorage))continue;for(const p of e.localStorage){if(typeof p.name==='string'&&typeof p.value==='string')localStorage.setItem(p.name,p.value);}}})()";
                    cdp->call("Page.addScriptToEvaluateOnNewDocument", {{"source", restore}}, session, token);
                }
                if (account.auth_user.empty() || !std::all_of(account.auth_user.begin(), account.auth_user.end(), [](unsigned char c) { return std::isdigit(c); })) throw Error(400, "invalid_auth_user", "auth_user must be a nonnegative integer");
                cdp->call("Page.navigate", {{"url", "https://aistudio.google.com/prompts/new_chat?authuser=" + account.auth_user}}, session, token);
                await_account_identity(account, token, !identity_only);
                account_id = account.id; account_fingerprint = std::move(fingerprint); last_status = "account_ready";
                verified_session = true;
            } catch (...) { try { dispose_context({}); } catch (...) { close(); } throw; }
        }
        if (!verified_session) await_account_identity(account, token, !identity_only);
        if (!bootstrap || !base_template.empty()) return;
        auto prepared = eval("globalThis.__ais2bridge.bootstrap()", token);
        if (!prepared.is_bool() || !prepared.as_bool()) incompatible("Cannot prepare the real page bootstrap prompt");
        bool template_seen = false;
        try {
            pause(token, 350ms);
            auto triggered = eval("globalThis.__ais2bridge.run()", token);
            if (!triggered.is_bool() || !triggered.as_bool()) incompatible("Cannot identify a supported page Run button");
            const auto deadline = Clock::now() + 45s;
            while (Clock::now() < deadline) {
                identity(token);
                auto capability = eval("globalThis.__ais2bridge.capability()", token);
                if (capability.is_object()) {
                    auto t = capability.as_object().if_contains("template");
                    if (t && t->is_object()) {
                        auto body = t->as_object().if_contains("body");
                        if (body && body->is_array()) {
                            template_seen = true;
                            if (bool_or(capability.as_object(), "snapshot")) {
                                base_template = body->as_array();
                                eval("globalThis.__ais2bridge.finishBootstrap()", token);
                                last_status = "gateway_ready"; return;
                            }
                        }
                    }
                }
                pause(token, 150ms);
            }
        } catch (...) {
            try { eval("globalThis.__ais2bridge.finishBootstrap()", {}, 2s); } catch (...) {}
            throw;
        }
        try { eval("globalThis.__ais2bridge.finishBootstrap()", {}, 2s); } catch (...) {}
        last_status = template_seen ? "bootstrap_snapshot_missing" : "bootstrap_template_missing";
        incompatible(template_seen
            ? "Live GenerateContent was captured but its snapshot adapter could not be verified"
            : "The page did not expose a supported fetch/XHR GenerateContent request during bootstrap");
    }
};

BrowserGateway::BrowserGateway(const Config& config, Store& store) : impl_(std::make_unique<Impl>(config, store)) {}
BrowserGateway::~BrowserGateway() = default;
json::object BrowserGateway::verify_runtime(std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    try {
        impl_->ensure_browser(cancel);
        auto version=impl_->cdp->call("Browser.getVersion",{}, {},cancel);
        auto context=string_or(impl_->cdp->call("Target.createBrowserContext",{{"disposeOnDetach",true}}, {},cancel),"browserContextId");
        if(context.empty())incompatible("Browser did not create a verification context");
        impl_->cdp->call("Target.disposeBrowserContext",{{"browserContextId",context}}, {},cancel);
        impl_->close();
        return {{"product",string_or(version,"product")},{"protocol_version",string_or(version,"protocolVersion")},{"isolated_context",true},{"google_verified",false}};
    } catch (...) { impl_->close();throw; }
}
void BrowserGateway::close() { std::lock_guard lock(impl_->mutex); impl_->close(); }
void BrowserGateway::reconfigure(const Config& config) {
    std::lock_guard lock(impl_->mutex);
    // Transport/lifecycle settings require a service restart. These fields are
    // the model policy that config.reload may atomically replace.
    impl_->config.configured_models = config.configured_models;
    impl_->config.model_aliases = config.model_aliases;
    impl_->config.model_defaults = config.model_defaults;
}
json::object BrowserGateway::diagnostics() {
    std::lock_guard lock(impl_->mutex);
    return {{"status", impl_->last_status}, {"running", impl_->process.alive()}, {"isolatedContext", !impl_->context.empty()}, {"templateCaptured", !impl_->base_template.empty()}, {"modelDiscovery", impl_->cached_models.empty() ? "not_loaded" : "live_rpc_cache"}, {"chunkBytes", 32768}};
}
json::object BrowserGateway::probe(const Account& account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, false, true);
    auto result = impl_->identity(cancel); result["accountId"] = account.id;
    result["ready"] = bool_or(result, "identityReady");
    result["status"] = string_or(result, "email").empty() ? "identity_unverified" : "ready";
    result["generationVerified"] = false;
    return result;
}
json::object BrowserGateway::probe_import(Account& account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, false, true);
    const auto info = impl_->identity(cancel);
    const auto email = string_or(info, "email");
    if (!bool_or(info, "identityReady") || bool_or(info, "identityAmbiguous") || email.empty())
        throw Error(409, "identity_unverified", "Import requires a ready AI Studio page with one verified Google identity");
    auto state = impl_->cdp->call("Storage.getCookies", {{"browserContextId", impl_->context}}, {}, cancel);
    auto cookies = state.if_contains("cookies");
    if (!cookies || !cookies->is_array()) incompatible("Browser did not return context cookies");
    std::size_t skipped_partition_cookies = 0;
    auto refreshed = browser_detail::import_cookies(cookies->as_array(), &skipped_partition_cookies);
    if (refreshed.empty()) throw Error(401, "missing_cookies", "Verified browser context has no Google cookies");
    check_cancel(cancel);
    account.storage_state["cookies"] = std::move(refreshed);
    impl_->account_fingerprint = sha256(json::serialize(account.storage_state) + "\n" + account.auth_user + "\n" + account.email);
    return {{"accountId", account.id}, {"email", email}, {"ready", true}, {"login", false}, {"identityAmbiguous", false}, {"status", "ready"}, {"generationVerified", false}, {"skippedPartitionCookies", skipped_partition_cookies}};
}
json::array BrowserGateway::models(std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    check_cancel(cancel);
    auto configured = [&] {
        json::array result;
        for (const auto& value : impl_->config.configured_models) {
            json::object item;
            if (value.is_object()) item = value.as_object();
            else if (value.is_string()) { item["id"] = value; item["name"] = "models/" + std::string(value.as_string()); }
            else continue;
            item["source"] = "configured"; result.push_back(std::move(item));
        }
        return result;
    };
    try {
        const auto account = impl_->store.choose("model_discovery");
        if (!account) return configured();
        // Listing models needs a verified account, never a generation request,
        // prompt editor, Run button or snapshot service. Recheck before cache.
        impl_->ensure_account(*account, cancel, false, true);
        if (!impl_->cached_models.empty() && Clock::now() - impl_->models_at < 600s) {
            auto cached = impl_->cached_models;
            for (auto& item : cached) item.as_object()["source"] = "cache";
            return cached;
        }
        auto result = impl_->eval("globalThis.__ais2bridge.discoverModels(" + quote_js(account->auth_user) + ")", cancel, 15s);
        if (!result.is_array()) incompatible("ListModels returned an unsupported response");
        impl_->cached_models = result.as_array(); impl_->models_at = Clock::now();
        return impl_->cached_models.empty() ? configured() : impl_->cached_models;
    } catch (const Error& error) {
        if (error.code == "cancelled") throw;
        auto result = configured();
        if (!result.empty()) return result;
        throw;
    }
}
void BrowserGateway::generate(const Request& request, const Account& account, const EventSink& sink, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, true);
    auto wire = encode_request(request, impl_->base_template, "");
    auto snapshot = impl_->eval("globalThis.__ais2bridge.snapshot(" + quote_js(content_hash(wire)) + ")", cancel, 15s);
    if (!snapshot.is_string() || snapshot.as_string().empty()) incompatible("Snapshot service produced no request ticket");
    wire[4] = snapshot;
    const auto body = json::serialize(wire);
    const auto chunk_bytes = std::min<std::size_t>(32768, impl_->config.limits.relay_bytes);
    if (!chunk_bytes) throw Error(500, "invalid_config", "Browser relay byte budget must be positive");
    impl_->eval("globalThis.__ais2bridge.start({body:" + quote_js(body) + ",authUser:" + quote_js(account.auth_user) + ",chunkBytes:" + std::to_string(chunk_bytes) + "})", cancel);
    WireStreamParser parser(impl_->config.limits.object_bytes);
    const auto began = Clock::now();
    auto activity = began;
    std::uint64_t sequence = 0;
    std::string error_prefix;
    constexpr std::size_t error_prefix_limit = 16384;
    std::optional<Error> parse_failure;
    try {
        while (true) {
            check_cancel(cancel);
            if (!request.stream && Clock::now() - began > std::chrono::seconds(impl_->config.limits.generation_seconds)) throw Error(504, "generation_timeout", "Generation deadline exceeded");
            if (Clock::now() - activity > std::chrono::seconds(impl_->config.limits.stream_idle_seconds)) throw Error(504, "stream_idle_timeout", "No upstream stream bytes before idle deadline");
            auto value = impl_->eval("globalThis.__ais2bridge.poll()", cancel, 5s);
            if (!value.is_object()) incompatible("Browser relay state was lost");
            const auto& state = value.as_object();
            const auto retry_value = state.if_contains("retryAfter");
            const auto retry_after = retry_value && retry_value->is_int64() && retry_value->as_int64() > 0 ? static_cast<unsigned>(std::min<std::int64_t>(86400, retry_value->as_int64())) : 0u;
            auto pending = state.if_contains("pending");
            if (pending && pending->is_object()) {
                auto number = pending->as_object().if_contains("sequence");
                if (!number || !number->is_int64() || number->as_int64() != static_cast<std::int64_t>(sequence + 1)) incompatible("Browser relay sequence mismatch");
                const auto bytes = decode64(string_or(pending->as_object(), "bytes"));
                if (bytes.empty()) incompatible("Empty browser relay credit");
                if (error_prefix.size() < error_prefix_limit) error_prefix.append(bytes.substr(0, error_prefix_limit - error_prefix.size()));
                if (auto error = browser_detail::rpc_text_error(error_prefix, retry_after)) throw *error;
                try { if (!parse_failure) parser.feed(bytes, [&](json::value chunk) {
                    check_cancel(cancel);
                    if (auto error = browser_detail::rpc_error(chunk, retry_after)) throw *error;
                    auto event = decode_chunk(chunk);
                    if (!event.empty()) sink(std::move(event));
                    check_cancel(cancel);
                }); } catch (const Error& error) {
                    if (error.code != "invalid_upstream" && error.code != "truncated_upstream") throw;
                    parse_failure = error;
                }
                if (parse_failure && error_prefix.size() >= error_prefix_limit) throw *parse_failure;
                if (!request.stream && Clock::now() - began > std::chrono::seconds(impl_->config.limits.generation_seconds)) throw Error(504, "generation_timeout", "Generation deadline exceeded while consuming downstream events");
                // EventSink returns only once downstream consumed its event.
                // ACK grants the page permission for exactly one next read.
                ++sequence;
                impl_->eval("globalThis.__ais2bridge.ack(" + std::to_string(sequence) + ")", cancel, 5s);
                activity = Clock::now();
            } else if (bool_or(state, "done")) {
                const auto error = string_or(state, "error");
                if (!error.empty()) {
                    if (auto rpc = browser_detail::rpc_text_error(string_or(state, "errorBody"), retry_after)) throw *rpc;
                    const auto status_value = state.if_contains("status");
                    const auto status = status_value && status_value->is_int64() ? status_value->as_int64() : 0;
                    if (auto http = browser_detail::rpc_status_error(status, retry_after)) throw *http;
                    throw Error(502, error.starts_with("browser_incompatible") ? "browser_incompatible" : "upstream_error", "Browser generation transport failed");
                }
                if (auto rpc = browser_detail::rpc_text_error(error_prefix, retry_after)) throw *rpc;
                if (parse_failure) throw *parse_failure;
                parser.finish(); return;
            } else pause(cancel, 30ms);
        }
    } catch (...) {
        try { impl_->eval("globalThis.__ais2bridge.abort()", {}, 3s); } catch (...) { impl_->close(); }
        throw;
    }
}
} // namespace ais2api
