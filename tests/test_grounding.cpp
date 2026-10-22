// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/protocol.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;
namespace {
json::object grounding_body(std::string_view tools) {
    auto body = json::parse(R"({"contents":[{"role":"user","parts":[{"text":"search this"}]}]})").as_object();
    body["tools"] = json::parse(tools);
    return body;
}
Request grounding_request(std::string_view tools, std::string model = "gemini-text") {
    return normalize_gemini(std::move(model), grounding_body(tools), false, Config{});
}
bool empty_slot(const json::array &wire, std::size_t index) {
    return wire.size() <= index || wire[index].is_null();
}
}

TEST_CASE("Grounding builtins match the upstream wire templates", "[protocol][grounding]") {
    auto request = grounding_request(R"([{"googleSearch":{},"codeExecution":{},"urlContext":{},"googleMaps":{}}])");
    auto wire = encode_request(request, {}, "snapshot", "Asia/Tokyo");
    auto &tools = wire[6].as_array();
    REQUIRE(tools.size() == 4);
    for (auto expected : {"[[]]", "[null,null,null,[null,[[]]]]",
                          "[null,null,null,null,null,null,null,[]]",
                          "[null,null,null,null,null,null,null,null,null,null,[]]"}) {
        bool found = false;
        for (auto &tool : tools) found = found || tool == json::parse(expected);
        REQUIRE(found);
    }
    REQUIRE(wire[13] == json::parse(R"([[null,null,"Asia/Tokyo"],null,1])"));
}

TEST_CASE("Builtin model gates reject unsupported tools without excluding functions", "[protocol][grounding]") {
    for (auto tools : {R"([{"codeExecution":{}}])", R"([{"googleMaps":{}}])", R"([{"urlContext":{}}])"})
        REQUIRE_THROWS_AS(grounding_request(tools, "gemini-image"), Error);
    for (auto model : {"gemma-test", "other-model"}) {
        for (auto tools : {R"([{"googleMaps":{}}])", R"([{"urlContext":{}}])"})
            REQUIRE_THROWS_AS(grounding_request(tools, model), Error);
        REQUIRE_NOTHROW(grounding_request(R"([{"googleSearch":{},"codeExecution":{}}])", model));
    }
    for (auto model : {"gemini-image", "gemma-test", "other-model"}) {
        auto wire = encode_request(grounding_request(R"([{"googleSearch":{},"functionDeclarations":[{"name":"lookup"}]}])", model), {}, "s");
        REQUIRE(wire[6].as_array().size() == 2);
    }
}

TEST_CASE("Image grounding combines split searches while text grounding uses web search", "[protocol][grounding]") {
    auto tools = R"([{"googleSearch":{"searchTypes":{"webSearch":{}}}},{"googleSearch":{"searchTypes":{"imageSearch":{}}}},{"googleSearch":{"searchTypes":{"webSearch":{},"imageSearch":{}}}},{"googleSearchRetrieval":{}}])";
    auto image = encode_request(grounding_request(tools, "gemini-image"), {}, "s");
    REQUIRE(image[6] == json::parse("[[null,null,null,[null,[[],[]]]]]"));
    REQUIRE(empty_slot(image, 13));
    auto text = encode_request(grounding_request(tools), {}, "s");
    REQUIRE(text[6] == json::parse("[[null,null,null,[null,[[]]]]]"));
    REQUIRE(text[13] == json::parse(R"([[null,null,"UTC"],null,1])"));
    auto image_only = encode_request(grounding_request(R"([{"googleSearch":{"searchTypes":{"imageSearch":{}}}}])", "gemini-image"), {}, "s");
    REQUIRE(image_only[6] == json::parse("[[null,null,null,[null,[null,[]]]]]"));
}

TEST_CASE("RikkaHub aliases retain mixed grounding tools and report unsupported controls", "[protocol][grounding]") {
    auto body = grounding_body(R"([{"google_search":{"search_types":{"web_search":{},"image_search":{}}},"code_execution":{},"url_context":{}},{"google_search_retrieval":{"dynamic_retrieval_config":{"mode":"MODE_DYNAMIC","dynamic_threshold":0.3}}}])");
    body["tool_config"] = json::parse(R"({"include_server_side_tool_invocations":true})");
    auto request = normalize_gemini("gemini-text", body, false, Config{});
    REQUIRE(request.ignored_parameters == json::parse(R"(["toolConfig.includeServerSideToolInvocations","tools.googleSearchRetrieval.dynamicRetrievalConfig"])"));
    auto wire = encode_request(request, {}, "s");
    REQUIRE(wire[6].as_array().size() == 3);
    REQUIRE(empty_slot(wire, 7));
    for (auto mode : {"MODE_DYNAMIC", "MODE_UNSPECIFIED"}) {
        auto legacy = grounding_body(R"([{"googleSearchRetrieval":{"dynamicRetrievalConfig":{"dynamicThreshold":0}}}])");
        legacy.at("tools").as_array()[0].as_object().at("googleSearchRetrieval").as_object().at("dynamicRetrievalConfig").as_object()["mode"] = mode;
        REQUIRE_NOTHROW(normalize_gemini("gemini-text", legacy, false, Config{}));
    }
}

TEST_CASE("Grounding rejects ambiguous aliases and invalid option values", "[protocol][grounding]") {
    for (auto tools : {
             R"([{"googleSearch":{"searchTypes":{},"search_types":{}}}])",
             R"([{"googleSearch":{"searchTypes":{"webSearch":{},"web_search":{}}}}])",
             R"([{"googleSearch":{"searchTypes":{"imageSearch":true}}}])",
             R"([{"googleSearchRetrieval":{"dynamicRetrievalConfig":{"mode":"OTHER"}}}])",
             R"([{"googleSearchRetrieval":{"dynamicRetrievalConfig":{"dynamicThreshold":-0.1}}}])",
             R"([{"googleSearchRetrieval":{"dynamicRetrievalConfig":{"dynamicThreshold":1.1}}}])",
             R"([{"googleSearchRetrieval":{"dynamicRetrievalConfig":{"dynamicThreshold":"0.5"}}}])",
             R"([{"googleSearchRetrieval":{"dynamicRetrievalConfig":{"dynamicThreshold":true}}}])",
             R"([{"googleSearchRetrieval":{"dynamicRetrievalConfig":{"dynamicThreshold":0.2,"dynamic_threshold":0.2}}}])"}) {
        INFO(tools);
        REQUIRE_THROWS_AS(grounding_request(tools), Error);
    }
    auto body = grounding_body(R"([{"googleSearch":{}}])");
    body["toolConfig"] = json::parse(R"({"includeServerSideToolInvocations":"true"})");
    REQUIRE_THROWS_AS(normalize_gemini("gemini-text", body, false, Config{}), Error);
    body["toolConfig"] = json::parse(R"({"includeServerSideToolInvocations":true,"include_server_side_tool_invocations":true})");
    REQUIRE_THROWS_AS(normalize_gemini("gemini-text", body, false, Config{}), Error);
}

TEST_CASE("Grounding deduplicates builtins and merges function declarations", "[protocol][grounding]") {
    auto request = grounding_request(R"([{"googleSearch":{},"codeExecution":{},"functionDeclarations":[{"name":"first"}]},{"googleSearchRetrieval":{},"codeExecution":{},"functionDeclarations":[{"name":"second"}]}])");
    auto wire = encode_request(request, {}, "s");
    REQUIRE(wire[6].as_array().size() == 3);
    std::size_t function_groups = 0;
    for (auto &tool : wire[6].as_array()) {
        auto &entry = tool.as_array();
        if (entry.size() > 1 && entry[1].is_array()) {
            ++function_groups;
            REQUIRE(entry[1] == json::parse(R"([["first"],["second"]])"));
        }
    }
    REQUIRE(function_groups == 1);
}

TEST_CASE("Grounding clears captured tools and location when disabled", "[protocol][grounding]") {
    json::array base;
    base.resize(14);
    base[6] = json::parse("[[[]]]");
    base[13] = json::parse(R"([[null,null,"captured-zone"],null,1])");
    auto omitted = grounding_body("[]");
    omitted.erase("tools");
    for (auto body : {omitted, grounding_body("[]")}) {
        auto wire = encode_request(normalize_gemini("gemini-text", body, false, Config{}), base, "s");
        REQUIRE(empty_slot(wire, 6));
        REQUIRE(empty_slot(wire, 13));
    }
    auto disabled = grounding_body(R"([{"googleSearch":{}}])");
    disabled["toolConfig"] = json::parse(R"({"functionCallingConfig":{"mode":"NONE"}})");
    auto wire = encode_request(normalize_gemini("gemini-text", disabled, false, Config{}), base, "s");
    REQUIRE(empty_slot(wire, 6));
    REQUIRE(empty_slot(wire, 13));
    auto image = encode_request(grounding_request(R"([{"googleSearch":{}}])", "gemini-image"), base, "s");
    REQUIRE(empty_slot(image, 13));
}

TEST_CASE("Disabled builtins do not trigger model restrictions or survive in the wire", "[protocol][grounding]") {
    auto body = grounding_body(R"([{"googleMaps":{}},{"codeExecution":{}},{"urlContext":{}}])");
    body["toolConfig"] = json::parse(R"({"functionCallingConfig":{"mode":"NONE"}})");
    for (auto model : {"gemini-image", "gemma-test"}) {
        auto wire = encode_request(normalize_gemini(model, body, false, Config{}), {}, "s");
        REQUIRE(wire[6].is_null());
        REQUIRE((wire.size() <= 13 || wire[13].is_null()));
    }
}

TEST_CASE("OpenAI extension forwards native grounding options", "[protocol][grounding]") {
    auto body = json::parse(R"({"model":"gemini-image","messages":[{"role":"user","content":"Find a reference image"}],"ais2api":{"gemini":{"tools":[{"google_search":{"search_types":{"image_search":{}}}}],"tool_config":{"include_server_side_tool_invocations":false}}}})").as_object();
    auto request = normalize_openai(body, Config{});
    REQUIRE(request.ignored_parameters == json::parse(R"(["toolConfig.includeServerSideToolInvocations"])"));
    auto wire = encode_request(request, {}, "s");
    REQUIRE(wire[6] == json::parse("[[null,null,null,[null,[null,[]]]]]"));
    REQUIRE(empty_slot(wire, 13));
}
