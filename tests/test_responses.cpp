// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/responses.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
json::object read(std::string_view s) { return json::parse(s).as_object(); }
json::object chunk(std::string text) {
    return {
        {"candidates",
         json::array{json::object{
             {"content", json::object{{"role", "model"},
                                      {"parts", json::array{json::object{{"text", text}}}}}}}}}};
}
} // namespace
TEST_CASE("Responses requests translate instructions formats and tools", "[responses]") {
    auto in = read(
        R"({"model":"m","instructions":"root","input":[{"role":"user","content":[{"type":"input_text","text":"hi"}]}],"max_output_tokens":100,"reasoning":{"effort":"low"},"tools":[{"type":"function","name":"f","parameters":{"type":"object"},"strict":false}],"tool_choice":"auto","text":{"format":{"type":"json_object"}}})");
    auto chat = responses_to_chat(in);
    REQUIRE(chat.at("messages").as_array().size() == 2);
    REQUIRE(chat.at("max_completion_tokens") == 100);
    REQUIRE(chat.at("tools").as_array()[0].as_object().at("function").as_object().at("name") ==
            "f");
    REQUIRE_NOTHROW(normalize_openai(chat, Config{}));
    REQUIRE_FALSE(responses_options(in).if_contains("input"));
}
TEST_CASE("Responses reject unsupported state and input items", "[responses]") {
    for (
        auto text :
        {R"({"model":"m","input":"x","store":true})",
         R"({"model":"m","input":"x","background":true})",
         R"({"model":"m","input":"x","previous_response_id":"resp_old"})",
         R"({"model":"m","input":[{"type":"item_reference","id":"x"}]})",
         R"({"model":"m","input":"x","tools":[{"type":"web_search"}]})",
         R"({"model":"m","input":[{"role":"user","content":[{"type":"input_image","image_url":"file:///secret"}]}]})"})
        REQUIRE_THROWS_AS(responses_to_chat(read(text)), Error);
}
TEST_CASE("Responses function history preserves signatures without storage", "[responses]") {
    auto gemini = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"f","args":{"n":1}},"thoughtSignature":"sig"}]}}]})");
    auto response = responses_response(gemini, "m", "resp_test", {});
    auto history = response.at("output").as_array();
    auto call = history[0].as_object().at("call_id");
    history.push_back(
        json::object{{"type", "function_call_output"}, {"call_id", call}, {"output", "done"}});
    auto req = normalize_openai(responses_to_chat({{"model", "m"}, {"input", history}}), Config{});
    auto &part =
        req.gemini.at("contents").as_array()[0].as_object().at("parts").as_array()[0].as_object();
    REQUIRE(part.at("thoughtSignature") == "sig");
    REQUIRE(part.at("functionCall").as_object().at("args").as_object().at("n") == 1);
}
TEST_CASE("Responses usage incomplete status and incremental SSE lifecycles", "[responses]") {
    auto gemini = chunk("hello");
    gemini.at("candidates").as_array()[0].as_object()["finishReason"] = "MAX_TOKENS";
    gemini["usageMetadata"] = json::object{{"promptTokenCount", 10},
                                           {"candidatesTokenCount", 5},
                                           {"totalTokenCount", 15},
                                           {"thoughtsTokenCount", 2}};
    auto r = responses_response(gemini, "m", "resp_test", {});
    REQUIRE(r.at("status") == "incomplete");
    REQUIRE(
        r.at("usage").as_object().at("output_tokens_details").as_object().at("reasoning_tokens") ==
        2);
    ResponsesStream stream("m", "resp_test", {}, 8192);
    auto events = stream.start();
    auto created = events[0].as_object().at("response").as_object().at("created_at");
    auto first = stream.push(chunk("hel"));
    REQUIRE(first.size() == 3);
    REQUIRE(first.back().as_object().at("delta") == "hel");
    auto second = stream.push(chunk("lo"));
    REQUIRE(second.size() == 1);
    REQUIRE(second[0].as_object().at("delta") == "lo");
    auto end = stream.finish();
    REQUIRE(end.back().as_object().at("type") == "response.completed");
    REQUIRE(end.back().as_object().at("response").as_object().at("created_at") == created);
    std::uint64_t sequence = 0;
    for (auto *list : {&events, &first, &second, &end})
        for (auto &event : *list)
            REQUIRE(event.as_object().at("sequence_number").as_uint64() == sequence++);
    REQUIRE(stream.finish().empty());
}
TEST_CASE("Responses streams emit function calls failures and enforce limits", "[responses]") {
    ResponsesStream stream("m", "resp_test", {}, 8192);
    auto call = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"f","args":{"n":1}},"thoughtSignature":"sig"}]}}]})");
    auto events = stream.push(call);
    REQUIRE(events.back().as_object().at("type") == "response.function_call_arguments.delta");
    auto done = stream.finish();
    REQUIRE(done[0].as_object().at("type") == "response.function_call_arguments.done");
    ResponsesStream limited("m", "resp_limit", {}, 128);
    REQUIRE_THROWS_AS(limited.push(chunk(std::string(512, 'x'))), Error);
    auto failed = limited.fail(Error(413, "response_too_large", "too big"));
    REQUIRE(failed.back().as_object().at("type") == "response.failed");
    REQUIRE(
        failed.back().as_object().at("response").as_object().at("error").as_object().at("code") ==
        "response_too_large");
    REQUIRE(limited.finish().empty());
}
TEST_CASE("Responses stable discovery order and signed ids across repeated snapshots",
          "[responses]") {
    struct Directory {
        fs::path path = fs::temp_directory_path() / ("ais2api-responses-" + random_hex(8));
        Directory() { fs::create_directories(path); }
        ~Directory() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } dir;
    Store store(dir.path);
    ResponsesStream stream("m", "resp_stable", {}, 32768, &store, "principal");
    auto calls = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"f","args":{}}},{"functionCall":{"name":"f","args":{}}}]}}]})");
    auto first = stream.push(calls);
    auto a = first[2].as_object().at("item").as_object();
    auto b = first[4].as_object().at("item").as_object();
    REQUIRE(a.at("call_id") != b.at("call_id"));
    auto text = stream.push(chunk("answer"));
    REQUIRE(text[0].as_object().at("output_index") == 2);
    auto done = stream.finish();
    auto final = done.back().as_object().at("response").as_object().at("output").as_array();
    REQUIRE(final.size() == 3);
    REQUIRE(final[0].as_object().at("id") == a.at("id"));
    REQUIRE(final[0].as_object().at("call_id") == a.at("call_id"));
    REQUIRE(final[1].as_object().at("call_id") == b.at("call_id"));
}

TEST_CASE("Responses defaults and input media retain the supported codec contract", "[responses]") {
    auto result = responses_response(chunk("answer"), "m", "resp_defaults", {});
    REQUIRE(result.at("tools").as_array().empty());
    REQUIRE(result.at("tool_choice") == "auto");
    REQUIRE(result.at("parallel_tool_calls") == true);
    REQUIRE(result.at("instructions").is_null());
    REQUIRE(result.at("text").as_object().at("format").as_object().at("type") == "text");
    auto image = responses_to_chat(read(
        R"({"model":"m","input":[{"role":"user","content":[{"type":"input_text","text":"describe"},{"type":"input_image","image_url":"data:image/png;base64,AQID"}]}]})"));
    auto request = normalize_openai(image, Config{});
    auto &parts = request.gemini.at("contents").as_array()[0].as_object().at("parts").as_array();
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[1].as_object().at("inlineData").as_object().at("data") == "AQID");
    auto remote = responses_to_chat(read(
        R"({"model":"m","input":[{"role":"user","content":[{"type":"input_image","image_url":"https://example.com/image.png"}]}]})"));
    REQUIRE(remote.at("messages")
                .as_array()[0]
                .as_object()
                .at("content")
                .as_array()[0]
                .as_object()
                .at("image_url")
                .as_object()
                .at("url") == "https://example.com/image.png");
    auto schema = responses_to_chat(read(
        R"({"model":"m","input":"x","text":{"format":{"type":"json_schema","name":"answer","strict":false,"schema":{"type":"object","properties":{"answer":{"type":"string"}}}}}})"));
    REQUIRE(normalize_openai(schema, Config{})
                .gemini.at("generationConfig")
                .as_object()
                .at("responseMimeType") == "application/json");
    for (auto extra : {R"("parallel_tool_calls":false)", R"("metadata":{"key":42})"}) {
        REQUIRE_THROWS_AS(
            responses_to_chat(read(std::string(R"({"model":"m","input":"x",)") + extra + "}")),
            Error);
    }
}

TEST_CASE("Responses repeated explicit function calls keep one invocation", "[responses]") {
    auto call = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"id":"original-call","name":"f","args":{"n":1}},"thoughtSignature":"sig"}]}}]})");
    ResponsesStream stream("m", "resp_repeat", {}, 32768);
    auto first = stream.push(call);
    auto original = first[2].as_object().at("item").as_object();
    REQUIRE(stream.push(call).empty());
    auto done = stream.finish();
    auto output = done.back().as_object().at("response").as_object().at("output").as_array();
    REQUIRE(output.size() == 1);
    REQUIRE(output[0].as_object().at("call_id") == original.at("call_id"));
    REQUIRE(output[0].as_object().at("id") == original.at("id"));
    ResponsesStream conflicting("m", "resp_conflict", {}, 32768);
    conflicting.push(call);
    auto revised = call;
    revised.at("candidates")
        .as_array()[0]
        .as_object()
        .at("content")
        .as_object()
        .at("parts")
        .as_array()[0]
        .as_object()
        .at("functionCall")
        .as_object()["args"] = json::object{{"n", 2}};
    REQUIRE_THROWS_AS(conflicting.push(revised), Error);
    auto failure = conflicting.fail(Error(502, "upstream_protocol_error", "conflict"));
    REQUIRE(failure.back().as_object().at("type") == "response.failed");
    REQUIRE(failure.back()
                .as_object()
                .at("response")
                .as_object()
                .at("output")
                .as_array()[0]
                .as_object()
                .at("arguments") == R"({"n":1})");
}

TEST_CASE("Responses native text and reasoning signatures replay in their respective items",
          "[responses]") {
    auto native = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"thinking","thought":true,"thoughtSignature":"reasoning-sig"},{"text":"answer"},{"text":"","thoughtSignature":"text-sig"},{"inlineData":{"mimeType":"image/png","data":"AQID"}}]}}]})");
    auto result = responses_response(native, "m", "resp_native", {});
    auto request = normalize_openai(
        responses_to_chat({{"model", "m"}, {"input", result.at("output")}}), Config{});
    auto &turns = request.gemini.at("contents").as_array();
    REQUIRE(turns.size() == 2);
    REQUIRE(turns[0].as_object().at("parts").as_array()[0].as_object().at("thoughtSignature") ==
            "reasoning-sig");
    REQUIRE(turns[1].as_object().at("parts").as_array()[1].as_object().at("thoughtSignature") ==
            "text-sig");
    REQUIRE(turns[1]
                .as_object()
                .at("parts")
                .as_array()[2]
                .as_object()
                .at("inlineData")
                .as_object()
                .at("data") == "AQID");
}
