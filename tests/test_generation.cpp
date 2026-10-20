// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend.hpp"
#include "ais2api/runtime.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>

using namespace ais2api;
namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

namespace {
json::object text_event(std::string text) {
    json::array parts;
    parts.push_back(json::object{{"text", std::move(text)}});
    json::array candidates;
    candidates.push_back(json::object{
        {"index", 0}, {"content", json::object{{"role", "model"}, {"parts", parts}}}});
    return {{"candidates", candidates}, {"responseId", "synthetic-response"}};
}

json::object finish_event() {
    json::array candidates;
    candidates.push_back(json::object{{"index", 0}, {"finishReason", "STOP"}});
    return {{"candidates", candidates},
            {"usageMetadata", json::object{{"promptTokenCount", 4},
                                           {"candidatesTokenCount", 3},
                                           {"totalTokenCount", 7}}}};
}

struct BackendState {
    std::atomic<unsigned> calls{0};
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    json::object request;
    std::string account_id;
    std::vector<std::string> probed_accounts;
};

class SyntheticBackend final : public GenerationBackend {
public:
    explicit SyntheticBackend(std::shared_ptr<BackendState> state) : state_(std::move(state)) {}

    void generate(const Request& request, const Account& account, const EventSink& emit,
                  std::stop_token cancel) override {
        state_->calls.fetch_add(1);
        {
            std::lock_guard lock(state_->mutex);
            state_->request = request.gemini;
            state_->account_id = account.id;
        }
        emit(text_event("hello "));
        if (request.model == "synthetic-after-content-error")
            throw Error(502, "synthetic_upstream_failure", "Synthetic failure after content");
        if (request.model == "synthetic-cancel") {
            const auto deadline = Clock::now() + 3s;
            while (!cancel.stop_requested() && Clock::now() < deadline)
                std::this_thread::sleep_for(5ms);
            state_->cancelled = cancel.stop_requested();
            check_cancel(cancel);
            throw Error(504, "synthetic_cancel_timeout", "Cancellation did not reach backend");
        }
        emit(text_event("world"));
        emit(finish_event());
    }

    json::array models(std::stop_token = {}) override { return {}; }
    json::object probe(const Account& account, std::stop_token cancel = {}) override {
        check_cancel(cancel);
        {
            std::lock_guard lock(state_->mutex);
            state_->probed_accounts.push_back(account.id);
        }
        if (account.name == "Synthetic first")
            throw Error(401, "session_expired", "Synthetic account session expired");
        return {{"ok", true}, {"status", "ok"}};
    }
    json::object diagnostics() override { return {{"synthetic", true}}; }
    void close() override {}

private:
    std::shared_ptr<BackendState> state_;
};

void receive_timeout(tcp::socket& socket) {
#ifdef _WIN32
    DWORD timeout = 5000;
#else
    timeval timeout{5, 0};
#endif
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0)
        throw std::runtime_error("Cannot configure integration-test receive timeout");
}

struct GenerationServer {
    fs::path directory = fs::temp_directory_path() / ("ais2api-generation-" + random_hex(10));
    Config config;
    std::string key;
    std::shared_ptr<BackendState> state = std::make_shared<BackendState>();
    std::jthread thread;
    std::atomic<bool> failed{false};

    explicit GenerationServer(bool browser_file_present = false) {
        net::io_context io;
        tcp::acceptor available(io, {tcp::v4(), 0});
        config.port = available.local_endpoint().port();
        available.close();
        config.data_dir = directory / "data";
        config.config_file = directory / "config.json";
        // This is only a regular-file prerequisite for /ready. The injected
        // backend never launches this executable or prepares a real browser.
        if (browser_file_present) config.browser_executable = executable_path();
        config.limits.request_bytes = 16384;
        config.limits.object_bytes = 32768;
        config.limits.relay_bytes = 1024;
        config.limits.queue_seconds = 5;
        config.limits.generation_seconds = 5;
        config.limits.stream_idle_seconds = 5;
        config.limits.max_attempts = 3;
        fs::create_directories(directory);
        atomic_write(config.config_file, json::serialize(config.to_json()), true);
        {
            Store store(config.data_dir);
            key = string_or(store.create_key("Synthetic integration key"), "secret");
            // Two healthy accounts ensure an erroneous retry has somewhere to go.
            store.import_accounts(json::value("SAPISID=synthetic-first-account"), "Synthetic first");
            store.import_accounts(json::value("SAPISID=synthetic-second-account"), "Synthetic second");
        }
        auto backend = std::make_unique<SyntheticBackend>(state);
        thread = std::jthread([this, backend = std::move(backend)]() mutable {
            try { serve_with_backend(config, std::move(backend)); }
            catch (...) { failed = true; }
        });
        bool ready = false;
        for (unsigned i = 0; i < 100 && !failed; ++i) {
            try { ready = bool_or(local_call(config, {{"command", "status"}}), "running"); }
            catch (const Error&) {}
            if (ready) break;
            std::this_thread::sleep_for(20ms);
        }
        if (!ready) {
            try { local_call(config, {{"command", "stop"}}); } catch (...) {}
            if (thread.joinable()) thread.join();
            throw std::runtime_error("Synthetic generation service did not become ready");
        }
    }

    ~GenerationServer() {
        try { local_call(config, {{"command", "stop"}}); } catch (...) {}
        if (thread.joinable()) thread.join();
        std::error_code error;
        fs::remove_all(directory, error);
    }

    http::request<http::string_body> request(std::string target, std::string body) const {
        http::request<http::string_body> result(http::verb::post, target, 11);
        result.set(http::field::host, "127.0.0.1");
        result.set(http::field::content_type, "application/json");
        result.set(http::field::authorization, "Bearer " + key);
        result.body() = std::move(body);
        result.prepare_payload();
        return result;
    }

    http::response<http::string_body> call(std::string target, std::string body,
                                         http::verb method = http::verb::post) {
        net::io_context io;
        tcp::socket socket(io);
        socket.connect({net::ip::make_address("127.0.0.1"), config.port});
        receive_timeout(socket);
        auto outgoing = request(std::move(target), std::move(body));
        outgoing.method(method);
        http::write(socket, outgoing);
        boost::beast::flat_buffer input;
        http::response_parser<http::string_body> parser;
        parser.body_limit(65536);
        http::read(socket, input, parser);
        return parser.release();
    }
};

std::vector<std::string> sse_records(std::string_view body) {
    std::vector<std::string> records;
    std::size_t offset = 0;
    while (offset < body.size()) {
        auto end = body.find("\n\n", offset);
        if (end == std::string_view::npos) throw std::runtime_error("Truncated SSE record");
        auto record = body.substr(offset, end - offset);
        if (!record.starts_with("data: ")) throw std::runtime_error("Unexpected SSE record type");
        records.emplace_back(record.substr(6));
        offset = end + 2;
    }
    return records;
}

json::object await_operation(const Config& config, const json::object& queued) {
    const auto id = string_or(queued, "operation_id");
    if (id.empty()) throw std::runtime_error("Probe did not return an operation id");
    const auto deadline = Clock::now() + 3s;
    while (Clock::now() < deadline) {
        auto result = local_call(config, {{"command", "operation.status"}, {"id", id}});
        const auto status = string_or(result, "status");
        if (status == "done" || status == "failed") return result;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Synthetic probe operation did not complete");
}
}

TEST_CASE("Gemini unary HTTP preserves multiple turns and aggregates synthetic events", "[generation][http]") {
    GenerationServer server;
    auto response = server.call("/v1beta/models/synthetic-unary:generateContent", R"({
      "contents": [
        {"role":"user","parts":[{"text":"first question"}]},
        {"role":"model","parts":[{"text":"earlier answer","thoughtSignature":"synthetic-signature"}]},
        {"role":"user","parts":[{"text":"follow-up"}]}
      ]
    })");
    REQUIRE(response.result_int() == 200);
    auto result = json::parse(response.body()).as_object();
    auto& candidate = result.at("candidates").as_array()[0].as_object();
    REQUIRE(candidate.at("content").as_object().at("parts").as_array()[0].as_object().at("text") == "hello world");
    REQUIRE(candidate.at("finishReason") == "STOP");
    REQUIRE(result.at("usageMetadata").as_object().at("totalTokenCount") == 7);
    REQUIRE(server.state->calls == 1);
    {
        std::lock_guard lock(server.state->mutex);
        auto& turns = server.state->request.at("contents").as_array();
        REQUIRE(turns.size() == 3);
        REQUIRE(turns[1].as_object().at("parts").as_array()[0].as_object().at("thoughtSignature") == "synthetic-signature");
        REQUIRE(turns[2].as_object().at("parts").as_array()[0].as_object().at("text") == "follow-up");
        REQUIRE_FALSE(server.state->account_id.empty());
    }
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("OpenAI SSE emits content usage and one final DONE over real HTTP", "[generation][http]") {
    GenerationServer server;
    auto response = server.call("/v1/chat/completions", R"({"model":"synthetic-stream","messages":[{"role":"user","content":"question"}],"stream":true,"stream_options":{"include_usage":true}})");
    REQUIRE(response.result_int() == 200);
    REQUIRE(response[http::field::content_type].starts_with("text/event-stream"));
    auto records = sse_records(response.body());
    REQUIRE(records.size() == 5);
    REQUIRE(records.back() == "[DONE]");
    std::string content;
    bool usage_seen = false;
    std::string response_id;
    for (std::size_t i = 0; i + 1 < records.size(); ++i) {
        auto chunk = json::parse(records[i]).as_object();
        REQUIRE(chunk.at("object") == "chat.completion.chunk");
        if (response_id.empty()) response_id = string_or(chunk, "id");
        REQUIRE(string_or(chunk, "id") == response_id);
        auto& choices = chunk.at("choices").as_array();
        if (choices.empty()) {
            REQUIRE(i + 2 == records.size()); // Usage immediately precedes DONE.
            REQUIRE(chunk.at("usage").as_object().at("total_tokens") == 7);
            usage_seen = true;
        } else {
            REQUIRE(chunk.at("usage").is_null());
            auto& choice = choices[0].as_object();
            content += string_or(choice.at("delta").as_object(), "content");
        }
    }
    REQUIRE(content == "hello world");
    REQUIRE(usage_seen);
    REQUIRE(server.state->calls == 1);
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("OpenAI SSE omits standard usage when include_usage is not requested", "[generation][http]") {
    GenerationServer server;
    auto response = server.call("/v1/chat/completions", R"({"model":"synthetic-stream","messages":[{"role":"user","content":"question"}],"stream":true})");
    REQUIRE(response.result_int() == 200);
    auto records = sse_records(response.body());
    REQUIRE(records.back() == "[DONE]");
    for (std::size_t i = 0; i + 1 < records.size(); ++i) {
        auto chunk = json::parse(records[i]).as_object();
        REQUIRE_FALSE(chunk.if_contains("usage"));
    }
    REQUIRE(server.state->calls == 1);
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("An upstream 502 after first content is a terminal SSE error without retry", "[generation][http]") {
    GenerationServer server;
    auto response = server.call("/v1beta/models/synthetic-after-content-error:streamGenerateContent", R"({"contents":[{"role":"user","parts":[{"text":"question"}]}]})");
    REQUIRE(response.result_int() == 200); // Headers were already committed.
    auto records = sse_records(response.body());
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].find("hello ") != std::string::npos);
    auto error = json::parse(records[1]).as_object().at("error").as_object();
    REQUIRE(error.at("code") == 502);
    REQUIRE(error.at("reason") == "synthetic_upstream_failure");
    REQUIRE(response.body().find("[DONE]") == std::string::npos);
    REQUIRE(server.state->calls == 1);
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("Disconnecting an HTTP stream cancels the active generation backend", "[generation][http]") {
    GenerationServer server;
    net::io_context io;
    tcp::socket socket(io);
    socket.connect({net::ip::make_address("127.0.0.1"), server.config.port});
    receive_timeout(socket);
    auto request = server.request("/v1beta/models/synthetic-cancel:streamGenerateContent", R"({"contents":[{"parts":[{"text":"cancel me"}]}]})");
    http::write(socket, request);
    boost::beast::flat_buffer input;
    http::response_parser<http::buffer_body> parser;
    http::read_header(socket, input, parser);
    REQUIRE(parser.get().result_int() == 200);
    std::string received = boost::beast::buffers_to_string(input.data());
    std::array<char, 1024> buffer{};
    while (received.find("hello ") == std::string::npos && received.size() < 8192) {
        received.append(buffer.data(), socket.read_some(net::buffer(buffer)));
    }
    REQUIRE(received.find("hello ") != std::string::npos);
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    const auto deadline = Clock::now() + 2s;
    while (!server.state->cancelled && Clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    REQUIRE(server.state->cancelled);
    REQUIRE(server.state->calls == 1);
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("Probe-all records one expired session and continues probing healthy accounts", "[generation][management][http]") {
    GenerationServer server(true);
    REQUIRE(server.call("/ready", {}, http::verb::get).result_int() == 200);
    auto before = local_call(server.config, {{"command", "accounts.list"}});
    auto& accounts = before.at("accounts").as_array();
    REQUIRE(accounts.size() == 2);
    std::string first, second;
    for (auto& value : accounts) {
        auto& account = value.as_object();
        if (string_or(account, "name") == "Synthetic first") first = string_or(account, "id");
        if (string_or(account, "name") == "Synthetic second") second = string_or(account, "id");
    }
    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(second.empty());
    auto queued = local_call(server.config, {{"command", "accounts.probe"}, {"all", true}});
    REQUIRE(string_or(queued, "status") == "queued");
    auto operation = await_operation(server.config, queued);
    REQUIRE(string_or(operation, "status") == "done");
    auto& results = operation.at("result").as_object().at("accounts").as_array();
    REQUIRE(results.size() == 2);
    unsigned failures = 0, successes = 0;
    for (auto& value : results) {
        auto& result = value.as_object();
        if (string_or(result, "account_id") == first) {
            REQUIRE(string_or(result, "status") == "failed");
            auto& error = result.at("error").as_object();
            REQUIRE(error.at("status") == 401);
            REQUIRE(error.at("code") == "session_expired");
            ++failures;
        } else if (string_or(result, "account_id") == second) {
            REQUIRE(bool_or(result, "ok"));
            REQUIRE_FALSE(result.if_contains("error"));
            ++successes;
        } else FAIL("Probe result refers to an unknown synthetic account");
    }
    REQUIRE(failures == 1);
    REQUIRE(successes == 1);
    {
        std::lock_guard lock(server.state->mutex);
        REQUIRE(server.state->probed_accounts.size() == 2);
        REQUIRE(server.state->probed_accounts[0] != server.state->probed_accounts[1]);
    }
    auto after = local_call(server.config, {{"command", "accounts.list"}});
    for (auto& value : after.at("accounts").as_array()) {
        auto& account = value.as_object();
        REQUIRE(account.at("expired").is_bool());
        REQUIRE(account.at("expired").as_bool() == (string_or(account, "id") == first));
    }
    auto single = await_operation(server.config, local_call(
        server.config, {{"command", "accounts.probe"}, {"id", first}}));
    REQUIRE(string_or(single, "status") == "failed");
    REQUIRE(single.at("error").as_object().at("status") == 401);
    REQUIRE(single.at("error").as_object().at("code") == "session_expired");
    auto generated = server.call("/v1beta/models/synthetic-unary:generateContent",
                                R"({"contents":[{"parts":[{"text":"healthy account remains usable"}]}]})");
    REQUIRE(generated.result_int() == 200);
    {
        std::lock_guard lock(server.state->mutex);
        REQUIRE(server.state->account_id == second);
    }
    local_call(server.config, {{"command", "accounts.remove"}, {"id", second}});
    auto not_ready = server.call("/ready", {}, http::verb::get);
    REQUIRE(not_ready.result_int() == 503);
    REQUIRE(json::parse(not_ready.body()).as_object().at("ready") == false);
    REQUIRE_FALSE(server.failed);
}
