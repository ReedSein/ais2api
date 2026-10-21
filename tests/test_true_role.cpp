// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/protocol.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
json::object read(std::string_view value) { return json::parse(value).as_object(); }
const json::array &parts(const json::object &event) {
    return event.at("candidates").as_array()[0].as_object().at("content").as_object().at("parts").as_array();
}
json::object event(json::array values) {
    return {{"candidates", json::array{json::object{{"content", {{"role", "model"}, {"parts", values}}}}}}};
}
json::object message(const json::object &response) {
    return response.at("choices").as_array()[0].as_object().at("message").as_object();
}
}
TEST_CASE("True role keeps intermediate instructions and adjacent role boundaries", "[protocol][true-role]") {
    auto request = normalize_openai(read(R"({"model":"m","messages":[{"role":"system","content":"root"},{"role":"developer","content":"more root"},{"role":"user","content":"first"},{"role":"user","content":"second"},{"role":"system","content":"middle"},{"role":"assistant","content":"answer"},{"role":"assistant","content":"preset"},{"role":"developer","content":"tail"}]})"), Config{});
    auto wire = encode_request(request, json::array(14), "!fresh");
    auto &history = wire[1].as_array();
    REQUIRE(history.size() == 6);
    REQUIRE(history[0].as_array()[1] == "user");
    REQUIRE(history[1].as_array()[1] == "user");
    REQUIRE(history[2].as_array()[0].as_array()[0].as_array()[1] == "middle");
    REQUIRE(history[3].as_array()[1] == "model");
    REQUIRE(history[4].as_array()[1] == "model");
    REQUIRE(history[5].as_array()[0].as_array()[0].as_array()[1] == "tail");
    REQUIRE(wire[5].as_array()[1] == "user");
    REQUIRE(wire[5].as_array()[0].as_array().size() == 2);
    REQUIRE(wire[6].is_null());
    REQUIRE(wire[7].is_null());
    REQUIRE(wire[11].is_null());
}
TEST_CASE("Signed text boundaries survive aggregation and native history edits", "[protocol][true-role]") {
    json::array expected{json::object{{"text", "thinking"}, {"thought", true}},
                         json::object{{"text", "answer"}},
                         json::object{{"text", ""}, {"thoughtSignature", "original"}},
                         json::object{{"text", "after"}, {"thoughtSignature", "second"}},
                         json::object{{"text", "tail"}}};
    json::object aggregate;
    for (auto &part : expected) merge_event(aggregate, event(json::array{part}), 8192);
    REQUIRE(parts(aggregate) == expected);
    auto history = read(R"({"contents":[{"role":"user","parts":[{"text":"edited"}]},{"role":"model","parts":[]},{"role":"model","parts":[{"text":"preset"}]}]})");
    history.at("contents").as_array()[1].as_object()["parts"] = parts(aggregate);
    auto req = normalize_gemini("m", history, false, Config{});
    auto wire = encode_request(req, {}, "!fresh");
    auto &signed_parts = wire[1].as_array()[1].as_array()[0].as_array();
    REQUIRE(signed_parts.size() == 5);
    REQUIRE(signed_parts[2].as_array()[14] == "original");
    REQUIRE(signed_parts[3].as_array()[14] == "second");
}
TEST_CASE("OpenAI reasoning and independent signature roundtrip without native extension", "[protocol][true-role]") {
    auto native = event(json::parse(R"([{"text":"think","thought":true},{"text":"answer"},{"text":"","thoughtSignature":"sig"}])").as_array());
    auto msg = message(openai_response(native, "m", "response"));
    REQUIRE(msg.at("reasoning_content") == "think");
    REQUIRE(msg.at("thought_signature") == "sig");
    auto with_native = normalize_openai({{"model", "m"}, {"messages", json::array{msg}}}, Config{});
    REQUIRE(with_native.gemini.at("contents").as_array()[0].as_object().at("parts") == parts(native));
    msg.erase("ais2api");
    auto req = normalize_openai({{"model", "m"}, {"messages", json::array{msg}}}, Config{});
    REQUIRE(req.gemini.at("contents").as_array()[0].as_object().at("parts") == parts(native));
    msg["signature"] = "different";
    REQUIRE_THROWS_AS(normalize_openai({{"model", "m"}, {"messages", json::array{msg}}}, Config{}), Error);
    auto delta = openai_delta(native, "m", "stream");
    auto &d = delta.at("choices").as_array()[0].as_object().at("delta").as_object();
    REQUIRE(d.at("reasoning_content") == "think");
    REQUIRE(d.at("thought_signature") == "sig");
    REQUIRE(d.at("content") == "answer");
}
TEST_CASE("Tool signatures roundtrip in client ids without a Store", "[protocol][true-role]") {
    for (bool explicit_id : {false, true}) {
        auto native = event(json::parse(R"([{"functionCall":{"name":"f","args":{"n":1}},"thoughtSignature":"signature-f"},{"functionCall":{"name":"g","args":{}},"thoughtSignature":"signature-g"}])").as_array());
        if (explicit_id)
            native.at("candidates").as_array()[0].as_object().at("content").as_object().at("parts").as_array()[0].as_object().at("functionCall").as_object()["id"] = "original-id";
        auto msg = message(openai_response(native, "m", "response"));
        auto calls = msg.at("tool_calls").as_array();
        auto id = string_or(calls[0].as_object(), "id");
        REQUIRE(id.starts_with("call_gemini_"));
        auto roundtrip = normalize_openai({{"model", "m"}, {"messages", json::array{msg}}}, Config{});
        REQUIRE(roundtrip.gemini.at("contents").as_array()[0].as_object().at("parts") == parts(native));
        msg.erase("ais2api");
        auto req = normalize_openai({{"model", "m"}, {"messages", json::array{
            msg, json::object{{"role", "tool"}, {"tool_call_id", id}, {"content", "done"}},
            json::object{{"role", "tool"}, {"tool_call_id", calls[1].as_object().at("id")}, {"content", "{}"}}}}}, Config{});
        auto &history = req.gemini.at("contents").as_array();
        REQUIRE(history[0].as_object().at("parts") == parts(native));
        REQUIRE(history.size() == 2);
        auto &results = history[1].as_object().at("parts").as_array();
        REQUIRE(results.size() == 2);
        auto &result = results[0].as_object().at("functionResponse").as_object();
        REQUIRE(result.at("name") == "f");
        if (explicit_id) REQUIRE(result.at("id") == "original-id");
        else REQUIRE_FALSE(result.if_contains("id"));
    }
    REQUIRE_THROWS_AS(normalize_openai(read(R"({"model":"m","messages":[{"role":"assistant","tool_calls":[{"id":"call_gemini_invalid!","type":"function","function":{"name":"f","arguments":"{}"}}]}]})"), Config{}), Error);
}
TEST_CASE("Unknown signed payloads and malformed code fail explicitly", "[protocol][true-role]") {
    auto decode = [](json::array part) {
        json::array values;
        values.push_back(part);
        json::array candidates;
        candidates.push_back(json::array{json::array{values, "model"}, 1});
        json::array wire;
        wire.push_back(candidates);
        return decode_chunk(wire);
    };
    json::array unknown(15);
    unknown[6] = json::array{"unknown"};
    unknown[14] = "do not discard this signed payload";
    REQUIRE_THROWS_AS(decode(unknown), Error);
    json::array malformed(8);
    malformed[7] = json::array{1};
    REQUIRE_THROWS_AS(decode(malformed), Error);
    json::array signature(15);
    signature[1] = "";
    signature[14] = "signature-only";
    REQUIRE(parts(decode(signature))[0].as_object().at("thoughtSignature") == "signature-only");
}
TEST_CASE("Unsigned idless calls retain their native history shape", "[protocol][true-role]") {
    auto native = event(json::parse(R"([{"functionCall":{"name":"f","args":{}}}])").as_array());
    auto msg = message(openai_response(native, "m", "unsigned-response"));
    auto req = normalize_openai({{"model", "m"}, {"messages", json::array{msg}}}, Config{});
    REQUIRE(req.gemini.at("contents").as_array()[0].as_object().at("parts") == parts(native));
}
