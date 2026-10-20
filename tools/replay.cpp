// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend.hpp"
#include "ais2api/runtime.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace ais2api {
namespace {
namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;
constexpr unsigned concurrency = 4;
constexpr unsigned content_events = 8;
constexpr unsigned client_think_ms = 100;
constexpr std::size_t text_bytes = 1024;
constexpr std::size_t histogram_buckets = 512;
constexpr double histogram_ratio = 1.05;

std::uint64_t thread_cpu_nanoseconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        throw Error(500, "measurement", "Cannot sample backend thread CPU");
    auto ticks = [](FILETIME value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    timespec value{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
        throw Error(500, "measurement", "Cannot sample backend thread CPU");
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
#endif
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw Error(500, "measurement", "Cannot sample process CPU");
    auto ticks = [](FILETIME value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000000.0;
#else
    auto value = std::clock();
    if (value == static_cast<std::clock_t>(-1))
        throw Error(500, "measurement", "Cannot sample process CPU");
    return static_cast<double>(value) / CLOCKS_PER_SEC;
#endif
}

std::uint64_t rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        throw Error(500, "measurement", "Cannot sample process working set");
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t virtual_pages{}, resident_pages{};
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    if (!(statm >> virtual_pages >> resident_pages) || page_size <= 0)
        throw Error(500, "measurement", "Cannot sample /proc/self/statm resident pages");
    return resident_pages * static_cast<std::uint64_t>(page_size);
#endif
}

void maximize(std::atomic<std::uint64_t> &maximum, std::uint64_t value) {
    auto previous = maximum.load();
    while (previous < value && !maximum.compare_exchange_weak(previous, value)) {
    }
}

struct Measurements {
    std::atomic<std::uint64_t> requests{0}, successes{0}, errors{0};
    std::atomic<std::uint64_t> backend_cpu_ns{0}, rss_peak{0}, queue_peak{0};
    std::atomic<std::uint64_t> sample_errors{0};
    std::array<std::atomic<std::uint64_t>, histogram_buckets> histogram{};
    std::mutex errors_mutex;
    std::map<std::string, std::uint64_t> error_classes;
    void error(std::string category) {
        errors.fetch_add(1);
        std::lock_guard lock(errors_mutex);
        if (!error_classes.contains(category) && error_classes.size() >= 16)
            category = "other";
        ++error_classes[std::move(category)];
    }

    void latency(double milliseconds) {
        auto raw = std::ceil(std::log1p(milliseconds * 1000) / std::log(histogram_ratio));
        auto bucket = static_cast<std::size_t>(std::max(0.0, raw));
        histogram[std::min(bucket, histogram_buckets - 1)].fetch_add(1);
    }

    json::value percentile(double fraction) const {
        auto count = successes.load();
        if (!count)
            return nullptr;
        auto target = static_cast<std::uint64_t>(std::ceil(static_cast<double>(count) * fraction));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < histogram_buckets; ++i) {
            seen += histogram[i].load();
            if (seen >= target)
                return (std::pow(histogram_ratio, static_cast<double>(i)) - 1) / 1000;
        }
        return nullptr;
    }
};

class ReplayBackend final : public GenerationBackend {
  public:
    explicit ReplayBackend(Measurements &measurements) : measurements_(measurements) {}

    void generate(const Request &, const Account &, const EventSink &emit,
                  std::stop_token cancel) override {
        const auto begin = thread_cpu_nanoseconds();
        // The timer includes synthetic event creation and bounded sink waits,
        // but thread CPU excludes time asleep and work done by HTTP/client threads.
        auto record_cpu = [&] {
            auto end = thread_cpu_nanoseconds();
            measurements_.backend_cpu_ns.fetch_add(end >= begin ? end - begin : 0);
        };
        try {
            const std::string payload(text_bytes, 'x');
            for (unsigned i = 0; i < content_events; ++i) {
                check_cancel(cancel);
                json::array parts;
                parts.push_back(json::object{{"text", payload}});
                json::array candidates;
                candidates.push_back(json::object{
                    {"index", 0}, {"content", json::object{{"role", "model"}, {"parts", parts}}}});
                emit({{"candidates", candidates}});
            }
            json::array candidates;
            candidates.push_back(json::object{{"index", 0}, {"finishReason", "STOP"}});
            emit({{"candidates", candidates},
                  {"usageMetadata", json::object{{"promptTokenCount", 8},
                                                 {"candidatesTokenCount", 32},
                                                 {"totalTokenCount", 40}}}});
        } catch (...) {
            record_cpu();
            throw;
        }
        record_cpu();
    }

    json::array models(std::stop_token = {}) override { return {}; }
    json::object probe(const Account &, std::stop_token = {}) override {
        return {{"synthetic", true}};
    }
    json::object diagnostics() override { return {{"synthetic", true}}; }
    void close() override {}

  private:
    Measurements &measurements_;
};

void receive_timeout(tcp::socket &socket) {
#ifdef _WIN32
    DWORD timeout = 10000;
#else
    timeval timeout{10, 0};
#endif
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&timeout), sizeof(timeout)) != 0)
        throw Error(500, "socket_timeout", "Cannot configure replay receive timeout");
}

struct Fixture {
    fs::path root = fs::temp_directory_path();
    fs::path directory = root / ("ais2api-replay-" + random_hex(12));
    Config config;
    std::string key;
    std::jthread server;
    std::atomic<bool> failed{false}, exited{false};

    void start(Measurements &measurements) {
        net::io_context io;
        tcp::acceptor available(io, {tcp::v4(), 0});
        config.port = available.local_endpoint().port();
        available.close();
        config.data_dir = directory / "data";
        config.config_file = directory / "config.json";
        config.browser_executable = directory / "browser-not-used";
        config.limits.request_bytes = 16384;
        config.limits.object_bytes = 65536;
        config.limits.relay_bytes = 4096;
        config.limits.queue_size = 8;
        config.limits.queue_seconds = 10;
        config.limits.generation_seconds = 10;
        config.limits.stream_idle_seconds = 10;
        fs::create_directories(directory);
        atomic_write(config.config_file, json::serialize(config.to_json()), true);
        {
            Store store(config.data_dir);
            key = string_or(store.create_key("Ephemeral offline replay"), "secret");
            store.import_accounts(json::value("SAPISID=synthetic-offline-replay-cookie"));
        }
        auto backend = std::make_unique<ReplayBackend>(measurements);
        server = std::jthread([this, backend = std::move(backend)]() mutable {
            try {
                serve_with_backend(config, std::move(backend));
            } catch (...) {
                failed = true;
            }
            exited = true;
        });
        bool ready = false;
        for (unsigned i = 0; i < 100 && !failed; ++i) {
            try {
                ready = bool_or(local_call(config, {{"command", "status"}}), "running");
            } catch (const Error &) {
            }
            if (ready)
                return;
            std::this_thread::sleep_for(20ms);
        }
        throw Error(500, "server_start", "Offline replay listener did not become ready");
    }

    ~Fixture() {
        if (server.joinable()) {
            try {
                local_call(config, {{"command", "stop"}});
            } catch (...) {
            }
            server.join();
        }
        // Delete only this generated, test-owned directory under its known root.
        std::error_code error;
        auto target = fs::weakly_canonical(directory, error);
        auto parent = fs::weakly_canonical(root, error);
        if (!error && target.parent_path() == parent &&
            target.filename().string().starts_with("ais2api-replay-"))
            fs::remove_all(target, error);
    }
};

void validate_sse(std::string_view body) {
    std::size_t offset = 0, payload = 0;
    unsigned events = 0;
    bool stopped = false;
    while (offset < body.size()) {
        auto end = body.find("\n\n", offset);
        if (end == std::string_view::npos)
            throw Error(502, "sse_validation", "Truncated replay SSE");
        auto record = body.substr(offset, end - offset);
        if (!record.starts_with("data: "))
            throw Error(502, "sse_validation", "Invalid replay SSE prefix");
        auto event = json::parse(record.substr(6)).as_object();
        if (event.if_contains("error"))
            throw Error(502, "server_error", "Offline replay returned an error");
        auto &candidate = event.at("candidates").as_array()[0].as_object();
        if (auto content = candidate.if_contains("content")) {
            for (auto &part : content->as_object().at("parts").as_array())
                payload += string_or(part.as_object(), "text").size();
        }
        if (string_or(candidate, "finishReason") == "STOP")
            stopped = true;
        ++events;
        offset = end + 2;
    }
    if (events != content_events + 1 || payload != content_events * text_bytes || !stopped)
        throw Error(502, "sse_validation",
                    "Offline replay response did not match its fixed workload");
}

void one_request(const Fixture &fixture, net::io_context &io) {
    tcp::socket socket(io);
    socket.connect({net::ip::make_address("127.0.0.1"), fixture.config.port});
    receive_timeout(socket);
    http::request<http::string_body> request(
        http::verb::post, "/v1beta/models/synthetic-replay:streamGenerateContent", 11);
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::authorization, "Bearer " + fixture.key);
    request.set(http::field::content_type, "application/json");
    request.body() =
        R"({"contents":[{"role":"user","parts":[{"text":"synthetic offline replay"}]}]})";
    request.prepare_payload();
    http::write(socket, request);
    boost::beast::flat_buffer input;
    http::response_parser<http::string_body> response;
    response.body_limit(65536);
    http::read(socket, input, response);
    if (response.get().result_int() != 200)
        throw Error(502, "http_status", "Offline replay request failed");
    validate_sse(response.get().body());
}

json::object replay(unsigned seconds) {
    Measurements measurements;
    Fixture fixture;
    fixture.start(measurements);
    const auto first_rss = rss_bytes();
    measurements.rss_peak = first_rss;
    const auto first_cpu = process_cpu_seconds();
    const auto begin = Clock::now();
    const auto deadline = begin + std::chrono::seconds(seconds);
    std::jthread sampler([&](std::stop_token cancel) {
        while (!cancel.stop_requested()) {
            try {
                maximize(measurements.rss_peak, rss_bytes());
                auto status = local_call(fixture.config, {{"command", "status"}});
                if (auto q = status.if_contains("queued")) {
                    if (q->is_uint64())
                        maximize(measurements.queue_peak, q->as_uint64());
                    else if (q->is_int64() && q->as_int64() >= 0)
                        maximize(measurements.queue_peak,
                                 static_cast<std::uint64_t>(q->as_int64()));
                }
            } catch (...) {
                measurements.sample_errors.fetch_add(1);
            }
            std::this_thread::sleep_for(100ms);
        }
    });
    std::array<std::jthread, concurrency> clients;
    for (auto &client : clients)
        client = std::jthread([&](std::stop_token cancel) {
            net::io_context io;
            while (!cancel.stop_requested() && !fixture.exited && Clock::now() < deadline) {
                const auto start = Clock::now();
                measurements.requests.fetch_add(1);
                try {
                    one_request(fixture, io);
                    measurements.latency(
                        std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    measurements.successes.fetch_add(1);
                } catch (const boost::system::system_error &error) {
                    measurements.error(std::string(error.code().category().name()) + ":" +
                                       std::to_string(error.code().value()));
                } catch (const Error &error) {
                    measurements.error("application:" + error.code);
                } catch (...) {
                    measurements.error("internal");
                }
                // Bound connection churn independently of backend speed. With four
                // clients this admits at most forty fresh TCP connections/second.
                std::this_thread::sleep_for(std::chrono::milliseconds(client_think_ms));
            }
        });
    for (auto &client : clients)
        client.join();
    const auto end = Clock::now();
    sampler.request_stop();
    sampler.join();
    const auto last_rss = rss_bytes();
    maximize(measurements.rss_peak, last_rss);
    const auto duration = std::chrono::duration<double>(end - begin).count();
    json::object error_classes;
    for (auto &[category, count] : measurements.error_classes)
        error_classes[category] = count;
    return {{"scope", "offline_backend_replay"},
            {"requested_duration_seconds", seconds},
            {"duration_seconds", duration},
            {"requests", measurements.requests.load()},
            {"successes", measurements.successes.load()},
            {"errors", measurements.errors.load()},
            {"error_classes", std::move(error_classes)},
            {"requests_per_second", static_cast<double>(measurements.successes.load()) / duration},
            {"latency_ms", json::object{{"p50", measurements.percentile(0.50)},
                                        {"p95", measurements.percentile(0.95)},
                                        {"p99", measurements.percentile(0.99)},
                                        {"scope", "successful_requests"},
                                        {"upper_bucket_ratio", histogram_ratio}}},
            {"backend_cpu_seconds",
             static_cast<double>(measurements.backend_cpu_ns.load()) / 1000000000.0},
            {"process_cpu_seconds", process_cpu_seconds() - first_cpu},
            {"rss_bytes", json::object{{"first", first_rss},
                                       {"last", last_rss},
                                       {"peak_sampled", measurements.rss_peak.load()}}},
            {"queue_peak_sampled", measurements.queue_peak.load()},
            {"sample_interval_ms", 100},
            {"sample_errors", measurements.sample_errors.load()},
            {"workload", json::object{{"concurrency", concurrency},
                                      {"protocol", "gemini_sse"},
                                      {"client_think_ms", client_think_ms},
                                      {"content_events_per_request", content_events},
                                      {"text_bytes_per_event", text_bytes},
                                      {"finish_events_per_request", 1},
                                      {"connection_reuse", false},
                                      {"cancellation_load", false}}},
            {"browser", nullptr},
            {"browser_verified", false},
            {"google_verified", false},
            {"original_project_comparison", nullptr},
            {"server_failed", fixture.failed.load()},
            {"server_stopped_early", fixture.exited.load() && end < deadline}};
}

unsigned parse_seconds(int argc, char **argv) {
    if (argc == 1)
        return 120;
    std::string_view value;
    if (argc == 3 && std::string_view(argv[1]) == "--seconds")
        value = argv[2];
    else if (argc == 2 && std::string_view(argv[1]).starts_with("--seconds="))
        value = std::string_view(argv[1]).substr(10);
    else
        throw Error(400, "arguments", "Use --seconds N (default 120, stability run 7200)");
    unsigned seconds{};
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || seconds < 1 ||
        seconds > 86400)
        throw Error(400, "arguments", "seconds must be within 1..86400");
    return seconds;
}
} // namespace
} // namespace ais2api

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "ais2api_replay [--seconds N]\n"
                         "Offline synthetic backend -> real loopback Gemini SSE. Default 120 s; "
                         "long run 7200 s.\n";
            return 0;
        }
        auto report = ais2api::replay(ais2api::parse_seconds(argc, argv));
        auto failed =
            report.at("errors").as_uint64() != 0 || report.at("sample_errors").as_uint64() != 0 ||
            report.at("server_failed").as_bool() || report.at("server_stopped_early").as_bool();
        std::cout << ais2api::json::serialize(report) << '\n';
        return failed ? 1 : 0;
    } catch (...) {
        std::cerr << "{\"scope\":\"offline_backend_replay\",\"error\":\"replay_failed\","
                     "\"browser\":null}\n";
        return 1;
    }
}
