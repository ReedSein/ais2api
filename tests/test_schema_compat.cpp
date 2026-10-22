// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/schema.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;
namespace {
json::object schema_object(std::string_view text) { return json::parse(text).as_object(); }
void require_invalid_schema(const json::object &schema) {
    try {
        (void)prepare_schema(schema, nullptr, "schema", true);
        FAIL("Expected invalid schema");
    } catch (const UnsupportedSchema &) {
        FAIL("Malformed schema must not be treated as an unsupported capability");
    } catch (const Error &e) {
        REQUIRE(e.status == 400);
        REQUIRE(e.code == "invalid_argument");
    }
}
}

TEST_CASE("Schema compatibility drops only known validated keywords", "[schema][compat]") {
    auto schema = schema_object(R"({"type":"object","properties":{"value":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":9,"multipleOf":2}},"patternProperties":{"^x":{"type":"string"}},"additionalItems":false,"prefixItems":[{"type":"string"}],"contains":{"type":"number"},"minContains":1,"maxContains":3,"uniqueItems":true,"dependencies":{"x":["y"]},"dependentRequired":{"x":["y"]},"dependentSchemas":{"x":{"type":"object"}},"if":{"type":"object"},"then":{"type":"object"},"else":false,"unevaluatedProperties":false,"unevaluatedItems":false,"contentEncoding":"base64","contentMediaType":"text/plain","contentSchema":{"type":"string"},"readOnly":true,"writeOnly":false,"deprecated":true,"examples":[{}]})");
    REQUIRE_THROWS_AS(prepare_schema(schema), Error);
    json::array ignored;
    auto result = prepare_schema(schema, &ignored, "response_format.schema", true);
    REQUIRE(result.size() == 2);
    REQUIRE(result.at("properties").as_object().at("value").as_object().size() == 1);
    REQUIRE(ignored.size() == 25);
    REQUIRE(ignored.front().is_string());
}

TEST_CASE("Schema compatibility preserves established metadata conversions", "[schema][compat]") {
    auto schema = schema_object(R"({"type":"object","title":"Result","default":{},"additionalProperties":false})");
    REQUIRE(prepare_schema(schema) == prepare_schema(schema, nullptr, "schema", true));
    REQUIRE(prepare_schema(schema).at("description").is_string());
}

TEST_CASE("Schema compatibility rejects invalid inputs before dropping constraints", "[schema][compat]") {
    for (auto text : {
        R"({"type":"string","patern":"x"})",
        R"({"type":"string","readOnly":"yes"})",
        R"({"type":"number","multipleOf":0})",
        R"({"type":"array","minContains":-1})",
        R"({"type":"object","dependentRequired":{"x":[4]}})",
        R"({"type":"object","patternProperties":{"x":{"type":"string","patern":"x"}}})",
        R"({"type":"object","if":{"type":"string","minLength":"x"}})",
        R"({"$ref":"#/~2"})",
        R"({"$ref":"#/missing"})",
        R"({"$defs":{"x":{"type":"string"}},"$ref":"#/$defs/x","type":"number"})",
        R"({"type":["string",7]})",
        R"({"type":"invalid"})"})
        require_invalid_schema(schema_object(text));
}

TEST_CASE("Unsupported whole schema forms use a distinct capability error", "[schema][compat]") {
    for (auto text : {
        R"({})",
        R"({"$ref":"https://example.com/schema"})",
        R"({"$ref":"#"})",
        R"({"type":["string","number"]})",
        R"({"type":"object","properties":{"x":true}})",
        R"({"type":"array","items":[{"type":"string"}]})",
        R"({"type":"array","items":false})",
        R"({"anyOf":[true,{"type":"string"}]})"}) {
        auto schema = schema_object(text);
        REQUIRE_THROWS_AS(prepare_schema(schema), UnsupportedSchema);
        REQUIRE_THROWS_AS(prepare_schema(schema, nullptr, "schema", true), UnsupportedSchema);
    }
}

TEST_CASE("Schema compatibility preserves local ref lowering and expansion limits", "[schema][compat]") {
    auto local = schema_object(R"({"$defs":{"value":{"type":"string","readOnly":true}},"type":"object","properties":{"x":{"$ref":"#/$defs/value"}}})");
    auto result = prepare_schema(local, nullptr, "schema", true);
    REQUIRE(result.at("properties").as_object().at("x").as_object().at("type") == "string");
    json::object deep{{"type", "string"}};
    for (unsigned i = 0; i < 66; ++i)
        deep = json::object{{"type", "array"}, {"items", std::move(deep)}};
    require_invalid_schema(deep);
    json::object large{{"type", "string"}, {"description", std::string(8 * 1024 * 1024, 'x')}};
    require_invalid_schema(large);
}

TEST_CASE("Unsupported schema siblings cannot hide invalid references", "[schema][compat]") {
    for (auto text : {
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"#/missing"}}})",
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"#/~2"}}})",
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"https://example.com/bad schema"}}})",
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"https://example.com/%XX"}}})",
        R"({"$defs":{"x":{"type":"string"}},"type":"object","properties":{"a":true,"b":{"$ref":"#/$defs/x","type":"number"}}})",
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"#","type":"string"}}})"})
        require_invalid_schema(schema_object(text));
    REQUIRE_THROWS_AS(prepare_schema(schema_object(R"({"type":"object","properties":{"a":true,"b":{"$ref":"#"}}})"), nullptr, "schema", true), UnsupportedSchema);
}
