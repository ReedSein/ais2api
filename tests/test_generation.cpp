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
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <tuple>

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
    json::array wire;
    std::string account_id;
    std::vector<std::string> attempted_accounts;
    std::vector<std::string> probed_accounts;
};

class SyntheticBackend final : public GenerationBackend {
public:
    explicit SyntheticBackend(std::shared_ptr<BackendState> state) : state_(std::move(state)) {}

    void generate(const Request& request, const Account& account, const EventSink& emit,
                  std::stop_token cancel) override {
        const auto attempt = state_->calls.fetch_add(1) + 1;
        {
            std::lock_guard lock(state_->mutex);
            state_->request = request.gemini;
            state_->wire = encode_request(request, {}, "!synthetic");
            state_->account_id = account.id;
            state_->attempted_accounts.push_back(account.id);
        }
        if ((request.model == "synthetic-route-retry" && attempt < 3) || request.model == "synthetic-route-always")
            throw Error(404, "transient_rpc_route", "Synthetic upstream routing failure");
        if ((request.model == "synthetic-empty-once" && attempt == 1) || request.model == "synthetic-empty-always")
            throw Error(502, "upstream_no_content", "Synthetic empty response");
        if (request.model == "synthetic-ordinary-not-found")
            throw Error(404, "upstream_error", "Synthetic model missing");
        if (request.model == "synthetic-unknown-tier")
            throw Error(403, "benefit_tier_unconfirmed", "Synthetic unknown account benefit");
        if (request.model == "synthetic-model-rate" && attempt == 1) {
            Error error(429, "rate_limit", "Synthetic model quota");
            error.quota_scope = "minute";
            throw error;
        }
        if (request.model == "synthetic-cancel") {
            // emit waits for downstream acknowledgment and may itself throw on
            // disconnect. Observe cancellation before publishing the first event
            // so both that race and cancellation during the wait are recorded.
            std::stop_callback on_cancel(cancel, [state = state_] { state->cancelled = true; });
            emit(text_event("hello "));
            const auto deadline = Clock::now() + 3s;
            while (!cancel.stop_requested() && Clock::now() < deadline)
                std::this_thread::sleep_for(5ms);
            check_cancel(cancel);
            throw Error(504, "synthetic_cancel_timeout", "Cancellation did not reach backend");
        }
        if (request.model == "synthetic-native-parts") {
            auto thought = text_event("thinking");
            thought.at("candidates").as_array()[0].as_object().at("content").as_object()
                .at("parts").as_array()[0].as_object()["thought"] = true;
            emit(thought);
            emit(text_event("answer"));
            auto signature = text_event("");
            signature.at("candidates").as_array()[0].as_object().at("content").as_object()
                .at("parts").as_array()[0].as_object()["thoughtSignature"] = "synthetic-original-signature";
            emit(signature);
            emit(finish_event());
            return;
        }
        if (request.model == "synthetic-responses-tools") {
            auto calls = text_event("");
            calls.at("candidates").as_array()[0].as_object().at("content").as_object()["parts"] =
                json::array{json::object{{"functionCall", {{"name", "lookup"}, {"args", {{"q", "test"}}}}},
                                         {"thoughtSignature", "synthetic-tool-signature"}}};
            emit(calls);
            emit(finish_event());
            return;
        }
        emit(text_event("hello "));
        if (request.model == "synthetic-unfinished") return;
        if (request.model == "synthetic-after-content-error")
            throw Error(502, "synthetic_upstream_failure", "Synthetic failure after content");
        emit(text_event("world"));
        auto finished = finish_event();
        if (request.model == "synthetic-length")
            finished.at("candidates").as_array()[0].as_object()["finishReason"] = "MAX_TOKENS";
        if (request.model == "synthetic-unspecified")
            finished.at("candidates").as_array()[0].as_object()["finishReason"] = "FINISH_REASON_UNSPECIFIED";
        emit(finished);
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
        key = "generation-test-" + random_hex(24);
        config.api_key = key;
        atomic_write(config.config_file, json::serialize(config.to_json()), true);
        {
            Store store(config.data_dir);
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
                                         http::verb method = http::verb::post,
                                         std::string auth_header = "Authorization") {
        net::io_context io;
        tcp::socket socket(io);
        socket.connect({net::ip::make_address("127.0.0.1"), config.port});
        receive_timeout(socket);
        auto outgoing = request(std::move(target), std::move(body));
        outgoing.method(method);
        if (auth_header != "Authorization") {
            outgoing.erase(http::field::authorization);
            if (!auth_header.empty()) outgoing.set(auth_header, key);
        }
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

TEST_CASE("Cherry style schemas reach the backend across Gemini Chat and Responses in unary and SSE", "[generation][http][schema]") {
    GenerationServer server;
    auto schema = json::parse(R"({"type":"object","title":"Search input","$defs":{"Query":{"type":"string","default":"hello"}},"properties":{"query":{"$ref":"#/$defs/Query"}},"required":["query"],"additionalProperties":false})");
    for (bool streaming : {false, true}) {
        for (auto protocol : {"gemini", "chat", "responses"}) {
            const std::string kind(protocol);
            json::object body;
            std::string route;
            if (kind == "gemini") {
                body = {{"contents", json::parse(R"([{"parts":[{"text":"hello"}]}])")},
                        {"tools", json::array{json::object{{"functionDeclarations", json::array{json::object{{"name", "search"}, {"parametersJsonSchema", schema}}}}}}},
                        {"generationConfig", json::object{{"responseMimeType", "application/json"}, {"responseJsonSchema", schema}}}};
                route = "/v1beta/models/synthetic-schema:" + std::string(streaming ? "streamGenerateContent?alt=sse" : "generateContent");
            } else if (kind == "chat") {
                body = {{"model", "synthetic-schema"}, {"stream", streaming},
                        {"messages", json::parse(R"([{"role":"user","content":"hello"}])")},
                        {"tools", json::array{json::object{{"type", "function"}, {"function", json::object{{"name", "search"}, {"parameters", schema}, {"strict", false}}}}}},
                        {"response_format", json::object{{"type", "json_schema"}, {"json_schema", json::object{{"name", "answer"}, {"strict", false}, {"schema", schema}}}}}};
                route = "/v1/chat/completions";
            } else {
                body = {{"model", "synthetic-schema"}, {"stream", streaming}, {"input", "hello"},
                        {"tools", json::array{json::object{{"type", "function"}, {"name", "search"}, {"parameters", schema}, {"strict", false}}}},
                        {"text", json::object{{"format", json::object{{"type", "json_schema"}, {"name", "answer"}, {"strict", false}, {"schema", schema}}}}}};
                route = "/v1/responses";
            }
            INFO(kind << ", stream=" << streaming);
            auto response = server.call(route, json::serialize(body));
            REQUIRE(response.result_int() == 200);
            REQUIRE(response[http::field::content_type].starts_with(streaming ? "text/event-stream" : "application/json"));
            std::lock_guard lock(server.state->mutex);
            const auto &wire = server.state->wire;
            const auto &parameters = wire[6].as_array()[0].as_array()[1].as_array()[0].as_array()[2];
            REQUIRE(parameters == wire[3].as_array()[8]);
            REQUIRE(parameters.as_array()[0] == 6);
            REQUIRE(parameters.as_array()[6].as_array()[0].as_array()[0] == "query");
            REQUIRE(parameters.as_array().size() <= 23);
        }
    }
    REQUIRE(server.state->calls == 6);
}

TEST_CASE("Compatibility config reload changes all data APIs without restarting", "[generation][http][compat]") {
    GenerationServer server;
    auto chat = json::parse(R"({"model":"synthetic-schema","messages":[{"role":"user","content":"hello"}],"parallel_tool_calls":false,"tool_choice":"required","response_format":{"type":"json_schema","json_schema":{"name":"reply","strict":true,"schema":{"type":"object","properties":{"n":{"type":"number","multipleOf":2}}}}}})").as_object();
    REQUIRE(server.call("/v1/chat/completions", json::serialize(chat)).result_int() == 400);
    REQUIRE(server.state->calls == 0);
    server.config.ignore_unsupported_parameters = true;
    atomic_write(server.config.config_file, json::serialize(server.config.to_json()), true);
    auto reload = local_call(server.config, {{"command", "config.reload"}});
    REQUIRE(reload.at("status") == "reloaded");
    for (bool streaming : {false, true}) {
        chat["stream"] = streaming;
        REQUIRE(server.call("/v1/chat/completions", json::serialize(chat)).result_int() == 200);
        auto responses = json::parse(R"({"model":"synthetic-schema","input":"hello","store":true,"background":true,"previous_response_id":"resp_old","parallel_tool_calls":false,"tools":[{"type":"web_search"}],"tool_choice":"required","text":{"format":{"type":"json_schema","name":"reply","strict":true,"schema":{"type":"string"}}}})").as_object();
        responses["stream"] = streaming;
        auto response = server.call("/v1/responses", json::serialize(responses));
        REQUIRE(response.result_int() == 200);
        REQUIRE(response.body().find("\"strict\":true") == std::string::npos);
        REQUIRE(response.body().find("\"background\":true") == std::string::npos);
        REQUIRE(response.body().find("web_search") == std::string::npos);
        REQUIRE(server.call(std::string("/v1beta/models/synthetic-schema:") + (streaming ? "streamGenerateContent?alt=sse" : "generateContent"),
            R"({"contents":[{"parts":[{"text":"hello"}]}],"generationConfig":{"candidateCount":2},"toolConfig":{"functionCallingConfig":{"mode":"ANY"}},"tools":[{"urlContext":{}}]})").result_int() == 200);
    }
    REQUIRE(server.state->calls == 6);
    server.config.ignore_unsupported_parameters = false;
    atomic_write(server.config.config_file, json::serialize(server.config.to_json()), true);
    local_call(server.config, {{"command", "config.reload"}});
    REQUIRE(server.call("/v1/chat/completions", json::serialize(chat)).result_int() == 400);
    REQUIRE(server.state->calls == 6);
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

TEST_CASE("RikkaHub and SillyTavern native request shapes work over HTTP", "[generation][http][gemini-clients]") {
    GenerationServer server;
    for (bool rikka : {true, false}) {
        for (bool streaming : {false, true}) {
            INFO("RikkaHub=" << rikka << ", streaming=" << streaming);
            auto body = json::parse(R"({"contents":[{"role":"user","parts":[{"text":"hello"}]}],
                "systemInstruction":{"parts":[{"text":"rules"}]},
                "safetySettings":[
                    {"category":"HARM_CATEGORY_HARASSMENT","threshold":"OFF"},
                    {"category":"HARM_CATEGORY_HATE_SPEECH","threshold":"OFF"},
                    {"category":"HARM_CATEGORY_SEXUALLY_EXPLICIT","threshold":"OFF"},
                    {"category":"HARM_CATEGORY_DANGEROUS_CONTENT","threshold":"OFF"},
                    {"category":"HARM_CATEGORY_CIVIC_INTEGRITY","threshold":"OFF"}]})").as_object();
            body["generationConfig"] = rikka
                ? json::parse(R"({"temperature":0.7,"thinkingConfig":{"includeThoughts":true,"thinkingLevel":"low","thinkingBudget":-1}})")
                : json::parse(R"({"candidateCount":1,"maxOutputTokens":128,"seed":42,"thinkingConfig":{"includeThoughts":false,"thinkingBudget":0},"responseModalities":["text","image"]})");
            if (!rikka)
                body["tools"] = json::parse(R"([{"function_declarations":[{"name":"lookup","parameters":{"type":"object","properties":{"query":{"type":"string"}}}}]},{"google_search":{}}])");
            auto target = std::string("/v1beta/models/gemini-synthetic-grounding:") +
                          (streaming ? "streamGenerateContent?alt=sse" : "generateContent");
            if (!rikka) target += (streaming ? "&key=" : "?key=") + server.key;
            auto response = server.call(target, json::serialize(body), http::verb::post,
                                        rikka ? "x-goog-api-key" : "");
            REQUIRE(response.result_int() == 200);
            json::object result;
            if (streaming) {
                REQUIRE(response[http::field::content_type].starts_with("text/event-stream"));
                for (const auto &record : sse_records(response.body())) {
                    REQUIRE(record != "[DONE]");
                    merge_event(result, json::parse(record).as_object(), 32768);
                }
            } else result = json::parse(response.body()).as_object();
            auto &candidate = result.at("candidates").as_array()[0].as_object();
            REQUIRE(candidate.at("content").as_object().at("parts").as_array()[0].as_object().at("text") == "hello world");
            REQUIRE(candidate.at("finishReason") == "STOP");
            std::lock_guard lock(server.state->mutex);
            REQUIRE(server.state->wire.at(2).as_array().size() == 4);
            if (!rikka) REQUIRE(server.state->wire.at(6).as_array().size() == 2);
        }
    }
    REQUIRE(server.state->calls == 4);
}

TEST_CASE("SillyTavern versioned proxy URLs generate native JSON and SSE", "[generation][http][gemini-clients]") {
    GenerationServer server;
    unsigned calls = 0;
    for (auto prefix : {"/v1beta/", "/v1beta/v1beta/", "/v1beta//v1beta/", "/v1beta//"}) {
        for (bool streaming : {false, true}) {
            INFO("prefix=" << prefix << ", streaming=" << streaming);
            // ST appends /v1beta/models/... even when the proxy URL already has /v1beta.
            // An encoded model id also checks extraction after prefix normalization.
            auto target = std::string(prefix) + "models/synthetic%2Dnative%2Dparts:" +
                (streaming ? "streamGenerateContent?alt=sse&key=" : "generateContent?key=") + server.key;
            auto response = server.call(target,
                R"({"contents":[{"parts":[{"text":"hello"}]}],"generationConfig":{"thinkingConfig":{"includeThoughts":false}}})",
                http::verb::post, "");
            REQUIRE(response.result_int() == 200);
            json::object result;
            if (streaming) {
                REQUIRE(response[http::field::content_type].starts_with("text/event-stream"));
                for (const auto& record : sse_records(response.body()))
                    merge_event(result, json::parse(record).as_object(), 32768);
            } else result = json::parse(response.body()).as_object();
            auto& candidate = result.at("candidates").as_array()[0].as_object();
            auto& parts = candidate.at("content").as_object().at("parts").as_array();
            REQUIRE(parts[0].as_object().at("text") == "answer");
            REQUIRE(parts.back().as_object().at("thoughtSignature") == "synthetic-original-signature");
            REQUIRE(candidate.at("finishReason") == "STOP");
            REQUIRE(server.state->calls == ++calls);
        }
    }
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("Client grounding options reach AI Studio wire over unary and SSE HTTP", "[generation][http][grounding]") {
    GenerationServer server;
    const std::vector<std::string> options{
        R"("tools":[{"functionDeclarations":[{"name":"lookup","parameters":{"type":"object"}}]},{"googleSearch":{}},{"urlContext":{}}],"toolConfig":{"includeServerSideToolInvocations":true})",
        R"("tools":[{"google_search":{}},{"code_execution":{}},{"google_search":{}}],"toolConfig":{"functionCallingConfig":{"mode":"AUTO"}})",
        R"("tools":[{"google_search_retrieval":{"dynamic_retrieval_config":{"mode":"MODE_DYNAMIC","dynamic_threshold":0.3}}}])"
    };
    for (std::size_t client = 0; client < options.size(); ++client) {
        for (bool streaming : {false, true}) {
            INFO("client=" << client << ", streaming=" << streaming);
            auto body = std::string(R"({"contents":[{"parts":[{"text":"search the web"}]}],)") + options[client] + "}";
            auto target = std::string("/v1beta/models/gemini-synthetic-grounding:") +
                          (streaming ? "streamGenerateContent?alt=sse" : "generateContent");
            auto response = server.call(target, body);
            REQUIRE(response.result_int() == 200);
            if (streaming) REQUIRE_FALSE(sse_records(response.body()).empty());
            else REQUIRE(json::parse(response.body()).as_object().if_contains("candidates"));
            std::lock_guard lock(server.state->mutex);
            auto &wire = server.state->wire;
            REQUIRE(wire.at(13) == json::parse(R"([[null,null,"UTC"],null,1])"));
            auto &tools = wire.at(6).as_array();
            REQUIRE(tools.size() == (client == 0 ? 3 : client == 1 ? 2 : 1));
            REQUIRE(std::count(tools.begin(), tools.end(), json::parse("[null,null,null,[null,[[]]]]")) == 1);
        }
    }
    REQUIRE(server.state->calls == 6);
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("includeThoughts filters text in unary and SSE while keeping signatures", "[generation][http][gemini-clients]") {
    GenerationServer server;
    for (bool streaming : {false, true}) {
        for (bool include : {false, true}) {
            auto body = json::parse(R"({"contents":[{"parts":[{"text":"question"}]}]})").as_object();
            body["generationConfig"] = json::object{{"thinkingConfig", json::object{{"includeThoughts", include}}}};
            auto target = std::string("/v1beta/models/synthetic-native-parts:") +
                          (streaming ? "streamGenerateContent?alt=sse" : "generateContent");
            auto response = server.call(target, json::serialize(body));
            REQUIRE(response.result_int() == 200);
            json::object result;
            if (streaming)
                for (const auto &record : sse_records(response.body()))
                    merge_event(result, json::parse(record).as_object(), 32768);
            else result = json::parse(response.body()).as_object();
            REQUIRE((json::serialize(result).find("thinking") != std::string::npos) == include);
            REQUIRE(json::serialize(result).find("answer") != std::string::npos);
            REQUIRE(json::serialize(result).find("synthetic-original-signature") != std::string::npos);
            REQUIRE(result.at("usageMetadata").as_object().at("totalTokenCount") == 7);
        }
    }
}

TEST_CASE("HTTP signatures survive streaming aggregation and next turn history", "[generation][http][true-role]") {
    GenerationServer server;
    for (bool streaming : {false, true}) {
        auto target = std::string("/v1beta/models/synthetic-native-parts:") +
                      (streaming ? "streamGenerateContent" : "generateContent");
        auto response = server.call(target, R"({"contents":[{"parts":[{"text":"question"}]}]})");
        REQUIRE(response.result_int() == 200);
        json::object result;
        unsigned signatures = 0;
        if (streaming) {
            for (const auto &record : sse_records(response.body())) {
                auto chunk = json::parse(record).as_object();
                if (auto cs = chunk.if_contains("candidates"))
                    for (auto &candidate : cs->as_array())
                        if (auto content = candidate.as_object().if_contains("content"))
                            for (auto &part : content->as_object().at("parts").as_array())
                                if (part.as_object().if_contains("thoughtSignature")) ++signatures;
                merge_event(result, chunk, 32768);
            }
            REQUIRE(signatures == 1);
        } else result = json::parse(response.body()).as_object();
        auto content = result.at("candidates").as_array()[0].as_object().at("content").as_object();
        auto &parts = content.at("parts").as_array();
        REQUIRE(parts.size() == 3);
        REQUIRE(parts[0].as_object().at("thought") == true);
        REQUIRE(parts[1].as_object().at("text") == "answer");
        REQUIRE(parts[2].as_object().at("text") == "");
        REQUIRE(parts[2].as_object().at("thoughtSignature") == "synthetic-original-signature");
        auto next = json::parse(R"({"contents":[{}, {"role":"user","parts":[{"text":"continue"}]}]})").as_object();
        next.at("contents").as_array()[0] = content;
        REQUIRE(server.call("/v1beta/models/synthetic-unary:generateContent", json::serialize(next)).result_int() == 200);
        std::lock_guard lock(server.state->mutex);
        REQUIRE(server.state->request.at("contents").as_array()[0] == json::value(content));
    }
    auto response = server.call("/v1/chat/completions", R"({"model":"synthetic-native-parts","messages":[{"role":"user","content":"question"}]})");
    REQUIRE(response.result_int() == 200);
    auto result = json::parse(response.body()).as_object();
    auto msg = result.at("choices").as_array()[0].as_object().at("message").as_object();
    REQUIRE(msg.at("reasoning_content") == "thinking");
    REQUIRE(msg.at("thought_signature") == "synthetic-original-signature");
    REQUIRE(msg.at("content") == "answer");
    msg.erase("ais2api");
    json::object next{{"model", "synthetic-unary"}, {"messages", json::array{msg, json::object{{"role", "user"}, {"content", "continue"}}}}};
    REQUIRE(server.call("/v1/chat/completions", json::serialize(next)).result_int() == 200);
    std::lock_guard lock(server.state->mutex);
    auto &restored = server.state->request.at("contents").as_array()[0].as_object().at("parts").as_array();
    REQUIRE(restored.size() == 3);
    REQUIRE(restored[2].as_object().at("thoughtSignature") == "synthetic-original-signature");
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

namespace {
json::array response_events(std::string_view body) {
    json::array events;
    while (!body.empty()) {
        auto end = body.find("\n\n");
        if (end == std::string_view::npos) throw std::runtime_error("Truncated Responses SSE record");
        auto record = body.substr(0, end);
        auto split = record.find("\ndata: ");
        if (!record.starts_with("event: ") || split == std::string_view::npos)
            throw std::runtime_error("Responses SSE requires event and data fields");
        auto event = json::parse(record.substr(split + 7)).as_object();
        REQUIRE(record.substr(7, split - 7) == string_or(event, "type"));
        REQUIRE(json::value_to<std::int64_t>(event.at("sequence_number")) ==
                static_cast<std::int64_t>(events.size()));
        events.push_back(std::move(event));
        body.remove_prefix(end + 2);
    }
    return events;
}
}

TEST_CASE("Responses JSON and named SSE expose consistent text and usage", "[generation][http][responses]") {
    GenerationServer server;
    for (bool streaming : {false, true}) {
        auto response = server.call("/v1/responses", json::serialize(json::object{
            {"model", "synthetic-stream"}, {"input", "question"},
            {"instructions", "Be concise"}, {"stream", streaming}, {"store", false}}));
        REQUIRE(response.result_int() == 200);
        json::object result;
        if (streaming) {
            REQUIRE(response[http::field::content_type].starts_with("text/event-stream"));
            auto events = response_events(response.body());
            REQUIRE(events.front().as_object().at("type") == "response.created");
            REQUIRE(events.back().as_object().at("type") == "response.completed");
            result = events.back().as_object().at("response").as_object();
            auto initial = events.front().as_object().at("response").as_object();
            REQUIRE(initial.at("id") == result.at("id"));
            REQUIRE(initial.at("created_at") == result.at("created_at"));
            std::string text;
            unsigned deltas = 0;
            for (const auto &value : events) {
                const auto &event = value.as_object();
                if (string_or(event, "type") == "response.output_text.delta") {
                    text += string_or(event, "delta");
                    ++deltas;
                }
            }
            REQUIRE(text == "hello world");
            REQUIRE(deltas == 2);
            REQUIRE(response.body().find("[DONE]") == std::string::npos);
        } else result = json::parse(response.body()).as_object();
        REQUIRE(result.at("object") == "response");
        REQUIRE(string_or(result, "id").starts_with("resp_"));
        REQUIRE(result.at("status") == "completed");
        REQUIRE(result.at("store") == false);
        const auto &message = result.at("output").as_array().back().as_object();
        REQUIRE(message.at("type") == "message");
        REQUIRE(message.at("role") == "assistant");
        REQUIRE(message.at("content").as_array()[0].as_object().at("text") == "hello world");
        REQUIRE(result.at("usage").as_object().at("input_tokens") == 4);
        REQUIRE(result.at("usage").as_object().at("output_tokens") == 3);
        REQUIRE(result.at("usage").as_object().at("total_tokens") == 7);
        std::lock_guard lock(server.state->mutex);
        REQUIRE(server.state->request.at("systemInstruction").as_object().at("parts")
                    .as_array()[0].as_object().at("text") == "Be concise");
    }
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("Responses function output replays signed tool history through HTTP", "[generation][http][responses]") {
    GenerationServer server;
    for (bool streaming : {false, true}) {
        auto response = server.call("/v1/responses", json::serialize(json::object{
            {"model", "synthetic-responses-tools"}, {"input", "question"}, {"stream", streaming}}));
        REQUIRE(response.result_int() == 200);
        auto result = streaming ? response_events(response.body()).back().as_object().at("response").as_object()
                                : json::parse(response.body()).as_object();
        auto output = result.at("output").as_array();
        auto call = std::find_if(output.begin(), output.end(), [](const json::value &value) {
            return string_or(value.as_object(), "type") == "function_call";
        });
        REQUIRE(call != output.end());
        REQUIRE(call->as_object().at("name") == "lookup");
        REQUIRE(string_or(call->as_object(), "call_id").starts_with("call_gemini_"));
        auto call_id = call->as_object().at("call_id");
        output.push_back(json::object{{"type", "function_call_output"}, {"call_id", call_id}, {"output", "found"}});
        auto next = server.call("/v1/responses", json::serialize(json::object{
            {"model", "synthetic-stream"}, {"input", output}}));
        REQUIRE(next.result_int() == 200);
        std::lock_guard lock(server.state->mutex);
        const auto &history = server.state->request.at("contents").as_array();
        REQUIRE(history[0].as_object().at("parts").as_array()[0].as_object().at("thoughtSignature") ==
                "synthetic-tool-signature");
        REQUIRE(history.back().as_object().at("parts").as_array()[0].as_object()
                    .at("functionResponse").as_object().at("name") == "lookup");
    }
}

TEST_CASE("Responses terminal states distinguish token exhaustion and upstream failure", "[generation][http][responses]") {
    GenerationServer server;
    for (bool streaming : {false, true}) {
        auto limited = server.call("/v1/responses", json::serialize(json::object{
            {"model", "synthetic-length"}, {"input", "question"}, {"stream", streaming}}));
        REQUIRE(limited.result_int() == 200);
        auto result = streaming ? response_events(limited.body()).back().as_object().at("response").as_object()
                                : json::parse(limited.body()).as_object();
        REQUIRE(result.at("status") == "incomplete");
        REQUIRE(result.at("incomplete_details").as_object().at("reason") == "max_output_tokens");
        if (streaming)
            REQUIRE(response_events(limited.body()).back().as_object().at("type") == "response.incomplete");
    }
    const auto before = server.state->calls.load();
    auto failed = server.call("/v1/responses", R"({"model":"synthetic-after-content-error","input":"question","stream":true})");
    REQUIRE(failed.result_int() == 200);
    auto events = response_events(failed.body());
    REQUIRE(events.back().as_object().at("type") == "response.failed");
    auto result = events.back().as_object().at("response").as_object();
    REQUIRE(result.at("status") == "failed");
    REQUIRE(result.at("error").as_object().at("code") == "synthetic_upstream_failure");
    REQUIRE(result.at("output").as_array()[0].as_object().at("status") == "incomplete");
    REQUIRE(failed.body().find("response.completed") == std::string::npos);
    REQUIRE(server.state->calls == before + 1);
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("Unsupported Responses state and tools fail before generation", "[generation][http][responses]") {
    GenerationServer server;
    for (const auto &extra : {R"("store":true)", R"("previous_response_id":"resp_other")",
                              R"("background":true)", R"("tools":[{"type":"web_search"}])",
                              R"("stream":"true")"}) {
        auto response = server.call("/v1/responses", std::string(R"({"model":"synthetic-stream","input":"question",)") + extra + "}");
        INFO(response.body());
        REQUIRE(response.result_int() == 400);
        REQUIRE(json::parse(response.body()).as_object().at("error").is_object());
    }
    REQUIRE(server.state->calls == 0);
}

TEST_CASE("Disconnecting Responses SSE cancels active generation", "[generation][http][responses]") {
    GenerationServer server;
    net::io_context io;
    tcp::socket socket(io);
    socket.connect({net::ip::make_address("127.0.0.1"), server.config.port});
    receive_timeout(socket);
    auto request = server.request("/v1/responses", R"({"model":"synthetic-cancel","input":"cancel me","stream":true})");
    http::write(socket, request);
    boost::beast::flat_buffer input;
    http::response_parser<http::buffer_body> parser;
    http::read_header(socket, input, parser);
    REQUIRE(parser.get().result_int() == 200);
    std::string received = boost::beast::buffers_to_string(input.data());
    std::array<char, 1024> buffer{};
    while (received.find("hello ") == std::string::npos && received.size() < 32768)
        received.append(buffer.data(), socket.read_some(net::buffer(buffer)));
    REQUIRE(received.find("hello ") != std::string::npos);
    SECTION("orderly shutdown") {
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
    }
    SECTION("connection reset") { socket.set_option(net::socket_base::linger(true, 0)); }
    socket.close();
    const auto deadline = Clock::now() + 2s;
    while (!server.state->cancelled && Clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    REQUIRE(server.state->cancelled);
    REQUIRE(server.state->calls == 1);
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

TEST_CASE("Recoverable route and first empty responses retry the same account within the attempt budget", "[generation][http][upstream-policy]") {
    for (const auto& [model, calls, status] : std::vector<std::tuple<std::string, unsigned, unsigned>>{
             {"synthetic-route-retry", 3, 200}, {"synthetic-route-always", 3, 404},
             {"synthetic-empty-once", 2, 200}, {"synthetic-empty-always", 2, 502},
             {"synthetic-ordinary-not-found", 1, 404}, {"synthetic-unknown-tier", 1, 403}}) {
        GenerationServer server;
        auto response = server.call("/v1beta/models/" + model + ":generateContent", R"({"contents":[{"parts":[{"text":"question"}]}]})");
        INFO(model);
        REQUIRE(response.result_int() == status);
        REQUIRE(server.state->calls == calls);
        const auto& attempts = server.state->attempted_accounts;
        REQUIRE(attempts.size() == calls);
        for (const auto& account : attempts) CHECK(account == attempts.front());
        Store store(server.config.data_dir);
        auto available = store.choose(model);
        REQUIRE(available); CHECK(available->id == attempts.front());
        CHECK_FALSE(server.failed);
    }
}

TEST_CASE("Quota failover cools only the failed model on the actual attempt account", "[generation][http][upstream-policy]") {
    GenerationServer server;
    const std::string model = "synthetic-model-rate";
    auto response = server.call("/v1beta/models/" + model + ":generateContent", R"({"contents":[{"parts":[{"text":"question"}]}]})");
    REQUIRE(response.result_int() == 200); REQUIRE(server.state->calls == 2);
    const auto& attempts = server.state->attempted_accounts;
    REQUIRE(attempts.size() == 2); REQUIRE(attempts[0] != attempts[1]);
    Store store(server.config.data_dir);
    CHECK_FALSE(store.choose(model, {attempts[1]}));
    auto other = store.choose("different-model", {attempts[1]});
    REQUIRE(other); CHECK(other->id == attempts[0]);
}

TEST_CASE("An unfinished response cannot succeed or restart after visible content", "[generation][http][upstream-policy]") {
    for (const std::string model : {"synthetic-unfinished", "synthetic-unspecified"}) {
        for (bool stream : {false, true}) {
            GenerationServer server;
            auto response = server.call("/v1beta/models/" + model + (stream ? ":streamGenerateContent" : ":generateContent"),
                                        R"({"contents":[{"parts":[{"text":"question"}]}]})");
            INFO(model); INFO(stream);
            REQUIRE(server.state->calls == 1);
            if (stream) {
                REQUIRE(response.result_int() == 200);
                auto records = sse_records(response.body());
                REQUIRE_FALSE(records.empty());
                const auto error = json::parse(records.back()).as_object().at("error").as_object();
                CHECK(error.at("reason") == "incomplete_upstream");
                CHECK(response.body().find("[DONE]") == std::string::npos);
            } else {
                REQUIRE(response.result_int() == 502);
                CHECK(response.body().find("incomplete_upstream") != std::string::npos);
            }
        }
    }
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
    SECTION("orderly shutdown") {
        socket.shutdown(tcp::socket::shutdown_both, ignored);
    }
    SECTION("connection reset") {
        // Force RST rather than relying on whether trailing SSE bytes remain
        // unread when the client closes its receive side.
        socket.set_option(net::socket_base::linger(true, 0));
    }
    socket.close(ignored);
    const auto deadline = Clock::now() + 2s;
    while (!server.state->cancelled && Clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    REQUIRE(server.state->cancelled);
    REQUIRE(server.state->calls == 1);
    REQUIRE_FALSE(server.failed);
}

TEST_CASE("Readiness requires a nonempty configured API key", "[generation][http][config]") {
    GenerationServer server(true);
    REQUIRE(server.call("/ready", {}, http::verb::get).result_int() == 200);
    server.config.api_key.clear();
    atomic_write(server.config.config_file, json::serialize(server.config.to_json()), true);
    local_call(server.config, {{"command", "config.reload"}});
    auto response = server.call("/ready", {}, http::verb::get);
    REQUIRE(response.result_int() == 503);
    REQUIRE_FALSE(bool_or(json::parse(response.body()).as_object(), "ready"));
    REQUIRE_FALSE(bool_or(local_call(server.config, {{"command", "status"}}), "ready"));
    REQUIRE(server.call("/v1/models", {}, http::verb::get).result_int() == 401);
    REQUIRE(server.state->calls == 0);
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
