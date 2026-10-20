// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/protocol.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;
namespace {
json::object obj(std::string_view text) { return json::parse(text).as_object(); }
json::array one(json::value value) {
    json::array a;
    a.push_back(std::move(value));
    return a;
}
json::value response_wire(json::array parts, json::value finish = nullptr) {
    json::array candidate{json::array{parts, "model"}, finish};
    json::array chunk{one(candidate), nullptr,
                      json::array{10, 2, 15, 1, nullptr, nullptr, nullptr, nullptr, nullptr, 3}};
    chunk.resize(8);
    chunk[7] = "r1";
    return chunk;
}
} // namespace

TEST_CASE("Gemini normalization preserves payloads and rejects extension conflicts", "[protocol]") {
    Config cfg;
    cfg.model_aliases["short"] = "gemini-test";
    auto request = normalize_gemini(
        "short",
        obj(R"({"contents":[{"role":"user","parts":[{"text":"hello"}]}],"ais2api":{"gemini":{"generationConfig":{"temperature":0.4}}}})"),
        true, cfg);
    REQUIRE(request.model == "gemini-test");
    REQUIRE(request.stream);
    REQUIRE(request.gemini.at("generationConfig").as_object().at("temperature") == 0.4);
    REQUIRE_THROWS_AS(
        normalize_gemini(
            "x",
            obj(R"({"contents":[{"parts":[{"text":"x"}]}],"generationConfig":{"temperature":1},"ais2api":{"gemini":{"generationConfig":{"temperature":1}}}})"),
            false, cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_gemini("x",
                         obj(R"({"contents":[{"parts":[{"fileData":{"fileUri":"files/a"}}]}]})"),
                         false, cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_gemini(
            "x",
            obj(R"({"contents":[{"parts":[{"text":"x","inlineData":{"mimeType":"image/png","data":"AA=="}}]}]})"),
            false, cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_gemini("x", obj(R"({"contents":[{"parts":[{"text":"x"}]}],"extra_body":{}})"),
                         false, cfg),
        Error);
}

TEST_CASE("JSPB contents contain typed Struct values and individual signatures", "[protocol]") {
    auto contents =
        json::parse(
            R"([{"role":"model","parts":[{"text":"a","thoughtSignature":"s1"},{"inlineData":{"mimeType":"image/png","data":"AA=="},"thoughtSignature":"s2"},{"functionCall":{"id":"c1","name":"calculate","args":{"enabled":true,"empty":[],"nested":{"n":2},"nil":null,"one":["a"]}},"thoughtSignature":"s3"}]}])")
            .as_array();
    auto encoded = encode_contents(contents);
    auto &parts = encoded[0].as_array()[0].as_array();
    REQUIRE(parts[0].as_array()[14] == "s1");
    REQUIRE(parts[1].as_array()[14] == "s2");
    // The response protocol places FunctionCall at slot 3, unlike request slot 10.
    json::array response_parts{parts[0], parts[1]};
    auto function = parts[2].as_array();
    function[3] = function[10];
    function[10] = nullptr;
    response_parts.push_back(function);
    auto decoded = decode_chunk(response_wire(response_parts, 1));
    auto &returned = decoded.at("candidates")
                         .as_array()[0]
                         .as_object()
                         .at("content")
                         .as_object()
                         .at("parts")
                         .as_array();
    REQUIRE(returned[0].as_object().at("thoughtSignature") == "s1");
    REQUIRE(returned[1].as_object().at("inlineData") ==
            contents[0].as_object().at("parts").as_array()[1].as_object().at("inlineData"));
    REQUIRE(returned[2].as_object().at("functionCall") ==
            contents[0].as_object().at("parts").as_array()[2].as_object().at("functionCall"));
    REQUIRE(decoded.at("usageMetadata").as_object().at("candidatesTokenCount") == 5);
    REQUIRE(decoded.at("usageMetadata").as_object().at("thoughtsTokenCount") == 3);
}

TEST_CASE("Schemas encode recursive properties union and constraints without metadata injection",
          "[protocol]") {
    auto s = encode_schema(obj(
        R"({"type":"object","properties":{"items":{"type":"array","items":{"type":["string","null"],"enum":["yes","no"]},"minItems":1},"count":{"type":"integer","minimum":0,"maximum":9}},"required":["items"],"propertyOrdering":["items","count"]})"));
    REQUIRE(s[0] == 6);
    REQUIRE(s.size() == 23);
    auto &entries = s[6].as_array();
    auto &item = entries[0].as_array()[1].as_array();
    REQUIRE(item[0] == 5);
    REQUIRE(item[5].as_array()[3] == true);
    REQUIRE(item[21] == 1);
    auto union_schema = encode_schema(obj(
        R"({"type":"string","oneOf":[{"type":"string","enum":["a"]},{"type":"string","enum":["b"]}],"example":"a"})"));
    REQUIRE(union_schema[16].as_array().size() == 2);
    REQUIRE(union_schema[15].as_array()[2] == "a");
    REQUIRE_THROWS_AS(encode_schema(obj(R"({"type":"object","additionalProperties":false})")),
                      Error);
    REQUIRE_THROWS_AS(encode_schema(obj(R"({"type":"string","title":"unsupported"})")), Error);
}

TEST_CASE("Request encoding replaces stale template generation fields", "[protocol]") {
    Config cfg;
    auto r = normalize_gemini(
        "gemini-test",
        obj(R"({"contents":[{"parts":[{"text":"new"}]}],"generationConfig":{"maxOutputTokens":19,"thinkingConfig":{"thinkingLevel":"LOW"}},"tools":[{"functionDeclarations":[{"name":"alpha","parametersJsonSchema":{"type":"object"}},{"name":"beta"}]}],"toolConfig":{"functionCallingConfig":{"mode":"AUTO","allowedFunctionNames":["alpha"]}}})"),
        false, cfg);
    json::array base;
    base.resize(14);
    base[5] = "stale";
    base[6] = "stale";
    base[11] = "stale cache";
    base[13] = json::array{1};
    auto wire = encode_request(r, base, "!fresh");
    REQUIRE(wire[0] == "models/gemini-test");
    REQUIRE(wire[4] == "!fresh");
    REQUIRE(wire[5].is_null());
    REQUIRE(wire[11].is_null());
    REQUIRE(wire[13] == json::array{1});
    REQUIRE(wire[3].as_array()[3] == 19);
    REQUIRE(wire[3].as_array()[16].as_array()[3] == 1);
    REQUIRE(wire[6].as_array()[0].as_array()[1].as_array().size() == 1);
}

TEST_CASE("Wire parser tolerates arbitrary byte splits and bounds the current chunk",
          "[protocol]") {
    auto chunk = response_wire(one(json::array{nullptr, "snow 雪 and \"quoted\" [ ]"}), 1);
    auto bytes =
        std::string(")]}'\n[[") + json::serialize(chunk) + "," + json::serialize(chunk) + "]]";
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        WireStreamParser parser(4096);
        std::vector<json::value> result;
        auto emit = [&](json::value v) { result.push_back(std::move(v)); };
        parser.feed(std::string_view(bytes).substr(0, split), emit);
        parser.feed(std::string_view(bytes).substr(split), emit);
        parser.finish();
        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == chunk);
        REQUIRE(parser.buffered_bytes() == 0);
    }
    WireStreamParser bytewise;
    std::size_t count = 0;
    for (char c : bytes)
        bytewise.feed(std::string_view(&c, 1), [&](json::value) { ++count; });
    bytewise.finish();
    REQUIRE(count == 2);
    WireStreamParser bounded(16);
    REQUIRE_THROWS_AS(bounded.feed(bytes, [](json::value) {}), Error);
    WireStreamParser truncated;
    truncated.feed("[[[null,", [](json::value) {});
    REQUIRE_THROWS_AS(truncated.finish(), Error);
    WireStreamParser malformed;
    REQUIRE_THROWS_AS(malformed.feed("[[[],]]", [](json::value) {}), Error);
    WireStreamParser invalid_utf8;
    std::string bad = "[[[\"";
    bad.push_back(static_cast<char>(0xff));
    bad += "\"]]]";
    REQUIRE_THROWS_AS(invalid_utf8.feed(bad, [](json::value) {}), Error);
}

TEST_CASE("Content gate ignores metadata and merge preserves part boundaries", "[protocol]") {
    json::object aggregate;
    json::object metadata{{"responseId", "id"},
                          {"usageMetadata", json::object{{"promptTokenCount", 2}}}};
    REQUIRE_FALSE(has_content(metadata));
    auto a = decode_chunk(response_wire(one(json::array{nullptr, "one"})));
    auto b = decode_chunk(response_wire(one(json::array{nullptr, "two"}), 1));
    REQUIRE(has_content(a));
    merge_event(aggregate, a, 4096);
    merge_event(aggregate, b, 4096);
    auto &parts = aggregate.at("candidates")
                      .as_array()[0]
                      .as_object()
                      .at("content")
                      .as_object()
                      .at("parts")
                      .as_array();
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0].as_object().at("text") == "onetwo");
    auto before = aggregate;
    REQUIRE_THROWS_AS(merge_event(aggregate, a, 4), Error);
    REQUIRE(aggregate == before);
}

TEST_CASE("OpenAI standard multimodal and tool messages normalize without wire aliases",
          "[protocol]") {
    Config cfg;
    auto r = normalize_openai(
        obj(R"({"model":"m","messages":[{"role":"system","content":"rules"},{"role":"user","content":[{"type":"text","text":"look"},{"type":"image_url","image_url":{"url":"data:image/png;base64,AA=="}}]},{"role":"assistant","content":null,"tool_calls":[{"id":"c1","type":"function","function":{"name":"f","arguments":"{\"a\":true}"}}]},{"role":"tool","tool_call_id":"c1","content":"{\"ok\":true}"}],"tools":[{"type":"function","function":{"name":"f","parameters":{"type":"object"}}}],"tool_choice":"auto","stream":true,"stream_options":{"include_usage":true},"ais2api":{"gemini":{"generationConfig":{"thinkingConfig":{"thinkingLevel":"HIGH"}}}}})"),
        cfg);
    REQUIRE(r.protocol == Protocol::openai);
    REQUIRE(r.stream);
    REQUIRE(r.include_usage);
    auto &contents = r.gemini.at("contents").as_array();
    REQUIRE(contents.size() == 3);
    REQUIRE(contents[0].as_object().at("parts").as_array().size() == 2);
    REQUIRE(contents[2]
                .as_object()
                .at("parts")
                .as_array()[0]
                .as_object()
                .at("functionResponse")
                .as_object()
                .at("name") == "f");
    REQUIRE_THROWS_AS(
        normalize_openai(
            obj(R"({"model":"m","messages":[{"role":"user","content":"x"}],"temperature":1,"ais2api":{"gemini":{"generationConfig":{"temperature":1}}}})"),
            cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_openai(obj(R"({"model":"m","messages":[{"role":"user","content":"x"}],"n":2})"),
                         cfg),
        Error);
}

TEST_CASE("OpenAI rich assistant roundtrip retains native image and thought signature",
          "[protocol]") {
    Config cfg;
    json::array text{nullptr, "answer"};
    text.resize(15);
    text[14] = "signed";
    auto gemini = decode_chunk(response_wire(
        json::array{text, json::array{nullptr, nullptr, json::array{"image/png", "AA=="}}}, 1));
    auto result = openai_response(gemini, "m", "chat-1");
    auto message = result.at("choices").as_array()[0].as_object().at("message");
    REQUIRE(message.as_object().at("content") == "answer");
    REQUIRE(message.as_object().at("ais2api").as_object().at("gemini_content").is_object());
    json::object request{
        {"model", "m"},
        {"messages",
         json::array{json::object{{"role", "user"}, {"content", "question"}}, message}}};
    auto normalized = normalize_openai(request, cfg);
    auto &parts = normalized.gemini.at("contents").as_array()[1].as_object().at("parts").as_array();
    REQUIRE(parts[0].as_object().at("thoughtSignature") == "signed");
    REQUIRE(parts.size() == 2);
    auto delta = openai_delta(gemini, "m", "chat-1");
    REQUIRE(delta.at("choices")
                .as_array()[0]
                .as_object()
                .at("delta")
                .as_object()
                .at("ais2api")
                .as_object()
                .at("gemini_delta") == gemini);
}

TEST_CASE("Compressed boolean and wrapped Struct lists decode without collapsing singleton values",
          "[protocol]") {
    json::array bool_value{nullptr, nullptr, nullptr, 1};
    json::array element{nullptr, nullptr, "single"};
    json::array list_value{nullptr, nullptr, nullptr, nullptr, nullptr, one(one(element))};
    json::array fields{json::array{"flag", bool_value}, json::array{"list", list_value}};
    json::array function{"f", one(one(fields)), "id"};
    json::array p{nullptr, nullptr, nullptr, function};
    auto decoded = decode_chunk(response_wire(one(p)));
    auto &args = decoded.at("candidates")
                     .as_array()[0]
                     .as_object()
                     .at("content")
                     .as_object()
                     .at("parts")
                     .as_array()[0]
                     .as_object()
                     .at("functionCall")
                     .as_object()
                     .at("args")
                     .as_object();
    REQUIRE(args.at("flag") == true);
    REQUIRE(args.at("list") == json::array{"single"});
}

TEST_CASE("OpenAI tools retain independent ids and native signatures through aggregation",
          "[protocol]") {
    json::array first{nullptr, nullptr, nullptr, json::array{"left", json::array{}, "left-id"}};
    first.resize(15);
    first[14] = "left-signature";
    json::array second{nullptr, nullptr, nullptr, json::array{"right", json::array{}, "right-id"}};
    second.resize(15);
    second[14] = "right-signature";
    auto event = decode_chunk(response_wire(json::array{first, second}, 1));
    auto response = openai_response(event, "m", "reply");
    auto &msg = response.at("choices").as_array()[0].as_object().at("message").as_object();
    auto &calls = msg.at("tool_calls").as_array();
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0].as_object().at("id") == "left-id");
    REQUIRE(calls[1].as_object().at("id") == "right-id");
    auto roundtrip =
        normalize_openai(json::object{{"model", "m"}, {"messages", json::array{msg}}}, Config{});
    auto &parts = roundtrip.gemini.at("contents").as_array()[0].as_object().at("parts").as_array();
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[0].as_object().at("thoughtSignature") == "left-signature");
    REQUIRE(parts[1].as_object().at("thoughtSignature") == "right-signature");
    auto delta = openai_delta(event, "m", "reply");
    auto &streamed = delta.at("choices")
                         .as_array()[0]
                         .as_object()
                         .at("delta")
                         .as_object()
                         .at("tool_calls")
                         .as_array();
    REQUIRE(streamed[0].as_object().at("index") == 0);
    REQUIRE(streamed[1].as_object().at("index") == 1);
}

TEST_CASE("Finish and usage convert to standard OpenAI semantics", "[protocol]") {
    auto event = decode_chunk(response_wire(one(json::array{nullptr, "partial"}), 2));
    auto response = openai_response(event, "m", "r");
    REQUIRE(response.at("choices").as_array()[0].as_object().at("finish_reason") == "length");
    auto &usage = response.at("usage").as_object();
    REQUIRE(usage.at("prompt_tokens") == 10);
    REQUIRE(usage.at("completion_tokens") == 5);
    REQUIRE(usage.at("completion_tokens_details").as_object().at("reasoning_tokens") == 3);
    auto blocked = openai_response(decode_chunk(response_wire({}, 3)), "m", "r");
    REQUIRE(blocked.at("choices").as_array()[0].as_object().at("finish_reason") ==
            "content_filter");
    auto nullable = encode_schema(obj(R"({"anyOf":[{"type":"null"},{"type":"string"}]})"));
    REQUIRE(nullable[0] == 1);
    REQUIRE(nullable[3] == true);
    REQUIRE(nullable[17].as_array().size() == 1);
}

TEST_CASE("Tool registry scopes signatures to principal and maintains stream call indices",
          "[protocol]") {
    struct Fixture {
        fs::path directory = fs::temp_directory_path() / ("ais2api-protocol-" + random_hex(10));
        std::unique_ptr<Store> store = std::make_unique<Store>(directory);
        ~Fixture() {
            store.reset();
            std::error_code ec;
            fs::remove_all(directory, ec);
        }
    } fixture;
    auto &store = *fixture.store;
    json::array first{nullptr, nullptr, nullptr, json::array{"f", json::array{}, "call-a"}};
    first.resize(15);
    first[14] = "sig-a";
    json::array second{nullptr, nullptr, nullptr, json::array{"g", json::array{}, "call-b"}};
    second.resize(15);
    second[14] = "sig-b";
    auto a =
        openai_delta(decode_chunk(response_wire(one(first))), "m", "response", &store, "key-one");
    auto b =
        openai_delta(decode_chunk(response_wire(one(second))), "m", "response", &store, "key-one");
    auto index = [](const json::object &delta) {
        return delta.at("choices")
            .as_array()[0]
            .as_object()
            .at("delta")
            .as_object()
            .at("tool_calls")
            .as_array()[0]
            .as_object()
            .at("index");
    };
    REQUIRE(index(a) == 0);
    REQUIRE(index(b) == 1);
    auto repeat =
        openai_delta(decode_chunk(response_wire(one(first))), "m", "response", &store, "key-one");
    REQUIRE(index(repeat) == 0);
    REQUIRE(store.get_tool("key-one", "call-a")->at("thoughtSignature") == "sig-a");
    REQUIRE_FALSE(store.get_tool("key-two", "call-a").has_value());
    auto finish =
        openai_delta(decode_chunk(response_wire({}, 1)), "m", "response", &store, "key-one");
    REQUIRE(finish.at("choices").as_array()[0].as_object().at("finish_reason") == "tool_calls");
    Config cfg;
    auto request = obj(
        R"({"model":"m","messages":[{"role":"assistant","content":null,"tool_calls":[{"id":"call-a","type":"function","function":{"name":"f","arguments":"{}"}}]}]})");
    auto restored = normalize_openai(request, cfg, &store, "key-one");
    REQUIRE(restored.gemini.at("contents")
                .as_array()[0]
                .as_object()
                .at("parts")
                .as_array()[0]
                .as_object()
                .at("thoughtSignature") == "sig-a");
    auto isolated = normalize_openai(request, cfg, &store, "key-two");
    REQUIRE_FALSE(isolated.gemini.at("contents")
                      .as_array()[0]
                      .as_object()
                      .at("parts")
                      .as_array()[0]
                      .as_object()
                      .if_contains("thoughtSignature"));
}

TEST_CASE("Unverified controls fail instead of advertising forced execution", "[protocol]") {
    Config cfg;
    REQUIRE_THROWS_AS(
        normalize_gemini(
            "m",
            obj(R"({"contents":[{"parts":[{"text":"x"}]}],"generationConfig":{"thinkingConfig":{"thinkingBudget":100}}})"),
            false, cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_gemini(
            "m",
            obj(R"({"contents":[{"parts":[{"text":"x"}]}],"generationConfig":{"thinkingConfig":{"includeThoughts":true}}})"),
            false, cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_openai(
            obj(R"({"model":"m","messages":[{"role":"user","content":"x"}],"tool_choice":"required"})"),
            cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_openai(
            obj(R"({"model":"m","messages":[{"role":"assistant","tool_calls":[{"id":"bad","type":"function"}]}]})"),
            cfg),
        Error);
    auto signed_only = normalize_gemini(
        "m", obj(R"({"contents":[{"role":"model","parts":[{"thoughtSignature":"opaque"}]}]})"),
        false, cfg);
    auto wire = encode_request(signed_only, {}, "!snapshot");
    REQUIRE(wire[1].as_array()[0].as_array()[0].as_array()[0].as_array()[14] == "opaque");
}

TEST_CASE("Executable code enums and function-response media survive request encoding",
          "[protocol]") {
    auto contents =
        json::parse(
            R"json([{"role":"user","parts":[{"functionResponse":{"name":"f","id":"c","response":{"ok":true},"parts":[{"text":"attachment"},{"inlineData":{"mimeType":"image/png","data":"AA=="},"thoughtSignature":"imgsig"}]}}]},{"role":"model","parts":[{"executableCode":{"language":"PYTHON","code":"print(2)"}},{"codeExecutionResult":{"outcome":"OUTCOME_FAILED","output":"failure"}},{"text":"reason","thought":true}]}])json")
            .as_array();
    auto encoded = encode_contents(contents);
    auto &attached = encoded[0].as_array()[0].as_array();
    REQUIRE(attached.size() == 3);
    REQUIRE(attached[2].as_array()[14] == "imgsig");
    auto &generated = encoded[1].as_array()[0].as_array();
    REQUIRE(generated[0].as_array()[8] == json::array{1, "print(2)"});
    REQUIRE(generated[1].as_array()[9] == json::array{2, "failure"});
    REQUIRE(generated[2].as_array()[12] == 1);
    auto decoded = decode_chunk(response_wire(generated));
    auto &result = decoded.at("candidates")
                       .as_array()[0]
                       .as_object()
                       .at("content")
                       .as_object()
                       .at("parts")
                       .as_array();
    REQUIRE(result[0].as_object().at("executableCode").as_object().at("language") == "PYTHON");
    REQUIRE(result[1].as_object().at("codeExecutionResult").as_object().at("outcome") ==
            "OUTCOME_FAILED");
    REQUIRE(result[2].as_object().at("thought") == true);
}

TEST_CASE("Singleton wire containers retain their exact protobuf layers", "[protocol]") {
    auto input =
        json::parse(
            R"([{"role":"model","parts":[{"functionCall":{"name":"f","id":"c","args":{"enabled":true}}}]}])")
            .as_array();
    auto encoded = encode_contents(input);
    auto &function = encoded[0].as_array()[0].as_array()[0].as_array()[10];
    REQUIRE(function == json::parse(R"(["f",[[["enabled",[null,null,null,true]]]],"c"])"));
    auto request = normalize_gemini(
        "m", obj(R"({"contents":[{"parts":[{"text":"x"}]}],"tools":[{"codeExecution":{}}]})"),
        false, Config{});
    auto wire = encode_request(request, {}, "!s");
    REQUIRE(wire[6] == json::parse("[[[]]]"));
    auto response = json::parse(
        R"([ [ [ [ [ [null,"text"] ], "model" ], 2 ] ], null, [1,2,3], null, null, null, null, "r" ])");
    auto decoded = decode_chunk(response);
    REQUIRE(has_content(decoded));
    REQUIRE(decoded.at("candidates").as_array()[0].as_object().at("finishReason") == "MAX_TOKENS");
}
namespace {
struct ToolRegressionStore {
    fs::path directory = fs::temp_directory_path() / ("ais2api-tools-regression-" + random_hex(8));
    std::unique_ptr<Store> store = std::make_unique<Store>(directory);
    ~ToolRegressionStore() {
        store.reset();
        std::error_code ec;
        fs::remove_all(directory, ec);
    }
};
json::object native_tool_event(std::optional<std::string> signature = {}, std::string id = {}) {
    json::object function{{"name", "same_function"}, {"args", {{"value", 7}}}};
    if (!id.empty())
        function["id"] = id;
    json::object part{{"functionCall", function}};
    if (signature)
        part["thoughtSignature"] = *signature;
    return {{"candidates",
             json::array{json::object{
                 {"index", 0}, {"content", {{"role", "model"}, {"parts", json::array{part}}}}}}}};
}
json::object stream_tool(const json::object &chunk) {
    return chunk.at("choices")
        .as_array()[0]
        .as_object()
        .at("delta")
        .as_object()
        .at("tool_calls")
        .as_array()[0]
        .as_object();
}
} // namespace
TEST_CASE("Repeated idless stream functions allocate independent calls and signatures",
          "[protocol][regression]") {
    ToolRegressionStore fixture;
    auto &store = *fixture.store;
    auto original_a = native_tool_event("signature-a");
    auto original_b = native_tool_event("signature-b");
    auto a = openai_delta(original_a, "m", "idless-stream", &store, "owner");
    auto b = openai_delta(original_b, "m", "idless-stream", &store, "owner");
    auto unsigned_call = openai_delta(native_tool_event(), "m", "idless-stream", &store, "owner");
    auto first = stream_tool(a), second = stream_tool(b), third = stream_tool(unsigned_call);
    REQUIRE(first.at("index") == 0);
    REQUIRE(second.at("index") == 1);
    REQUIRE(third.at("index") == 2);
    auto first_id = string_or(first, "id"), second_id = string_or(second, "id"),
         third_id = string_or(third, "id");
    REQUIRE(first_id != second_id);
    REQUIRE(second_id != third_id);
    REQUIRE(first_id != third_id);
    REQUIRE(store.get_tool("owner", first_id)->at("thoughtSignature") == "signature-a");
    REQUIRE(store.get_tool("owner", second_id)->at("thoughtSignature") == "signature-b");
    REQUIRE_FALSE(store.get_tool("owner", third_id)->if_contains("thoughtSignature"));
    REQUIRE_FALSE(store.get_tool("other-owner", first_id));
    auto &native = a.at("choices")
                       .as_array()[0]
                       .as_object()
                       .at("delta")
                       .as_object()
                       .at("ais2api")
                       .as_object()
                       .at("gemini_delta")
                       .as_object()
                       .at("candidates")
                       .as_array()[0]
                       .as_object()
                       .at("content")
                       .as_object()
                       .at("parts")
                       .as_array()[0]
                       .as_object();
    REQUIRE(native.at("functionCall").as_object().at("id") == first.at("id"));
    REQUIRE(native.at("thoughtSignature") == "signature-a");
    REQUIRE_FALSE(original_a.at("candidates")
                      .as_array()[0]
                      .as_object()
                      .at("content")
                      .as_object()
                      .at("parts")
                      .as_array()[0]
                      .as_object()
                      .at("functionCall")
                      .as_object()
                      .if_contains("id"));
    first.erase("index");
    second.erase("index");
    third.erase("index");
    json::object request{
        {"model", "m"},
        {"messages", json::array{json::object{{"role", "assistant"},
                                              {"content", nullptr},
                                              {"tool_calls", json::array{first, second, third}}}}}};
    auto restored = normalize_openai(request, Config{}, &store, "owner");
    auto &parts = restored.gemini.at("contents").as_array()[0].as_object().at("parts").as_array();
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0].as_object().at("thoughtSignature") == "signature-a");
    REQUIRE(parts[1].as_object().at("thoughtSignature") == "signature-b");
    REQUIRE_FALSE(parts[2].as_object().if_contains("thoughtSignature"));
}
TEST_CASE("Upstream tool ids retain their stream index among idless calls",
          "[protocol][regression]") {
    ToolRegressionStore fixture;
    auto &store = *fixture.store;
    auto anonymous =
        stream_tool(openai_delta(native_tool_event(), "m", "mixed-stream", &store, "owner"));
    auto named = stream_tool(openai_delta(native_tool_event("upstream-signature", "upstream-id"),
                                          "m", "mixed-stream", &store, "owner"));
    auto repeat = stream_tool(openai_delta(native_tool_event("upstream-signature", "upstream-id"),
                                           "m", "mixed-stream", &store, "owner"));
    auto next =
        stream_tool(openai_delta(native_tool_event(), "m", "mixed-stream", &store, "owner"));
    REQUIRE(anonymous.at("index") == 0);
    REQUIRE(named.at("id") == "upstream-id");
    REQUIRE(named.at("index") == 1);
    REQUIRE(repeat.at("id") == named.at("id"));
    REQUIRE(repeat.at("index") == named.at("index"));
    REQUIRE(next.at("index") == 2);
    REQUIRE(next.at("id") != anonymous.at("id"));
}
TEST_CASE("Signed idless function output roundtrips standard and native messages together",
          "[protocol][regression]") {
    json::object original = obj(
        R"json({"candidates":[{"content":{"role":"model","parts":[{"text":"answer","thoughtSignature":"text-signature"},{"functionCall":{"name":"left","args":{}},"thoughtSignature":"left-signature"},{"functionCall":{"name":"right","args":{"value":true}},"thoughtSignature":"right-signature"}]}}]})json");
    auto response = openai_response(original, "m", "stateless-response");
    auto message = response.at("choices").as_array()[0].as_object().at("message").as_object();
    auto &calls = message.at("tool_calls").as_array();
    REQUIRE(calls.size() == 2);
    auto left = string_or(calls[0].as_object(), "id"),
         right = string_or(calls[1].as_object(), "id");
    REQUIRE(left != right);
    auto &native =
        message.at("ais2api").as_object().at("gemini_content").as_object().at("parts").as_array();
    REQUIRE(native[1].as_object().at("functionCall").as_object().at("id") == json::value(left));
    REQUIRE(native[2].as_object().at("functionCall").as_object().at("id") == json::value(right));
    json::object request{
        {"model", "m"},
        {"messages",
         json::array{
             message,
             json::object{{"role", "tool"}, {"tool_call_id", left}, {"content", "{\"ok\":true}"}},
             json::object{{"role", "tool"}, {"tool_call_id", right}, {"content", "done"}}}}};
    auto roundtrip = normalize_openai(request, Config{});
    auto &contents = roundtrip.gemini.at("contents").as_array();
    auto &restored = contents[0].as_object().at("parts").as_array();
    REQUIRE(restored == native);
    REQUIRE(restored[0].as_object().at("thoughtSignature") == "text-signature");
    REQUIRE(restored[1].as_object().at("thoughtSignature") == "left-signature");
    REQUIRE(restored[2].as_object().at("thoughtSignature") == "right-signature");
    REQUIRE(contents[1]
                .as_object()
                .at("parts")
                .as_array()[0]
                .as_object()
                .at("functionResponse")
                .as_object()
                .at("id") == json::value(left));
    auto args_conflict = request;
    args_conflict.at("messages")
        .as_array()[0]
        .as_object()
        .at("tool_calls")
        .as_array()[0]
        .as_object()
        .at("function")
        .as_object()["arguments"] = "{\"changed\":true}";
    REQUIRE_THROWS_AS(normalize_openai(args_conflict, Config{}), Error);
    auto id_conflict = request;
    id_conflict.at("messages")
        .as_array()[0]
        .as_object()
        .at("tool_calls")
        .as_array()[0]
        .as_object()["id"] = "unrelated";
    REQUIRE_THROWS_AS(normalize_openai(id_conflict, Config{}), Error);
    REQUIRE_FALSE(original.at("candidates")
                      .as_array()[0]
                      .as_object()
                      .at("content")
                      .as_object()
                      .at("parts")
                      .as_array()[1]
                      .as_object()
                      .at("functionCall")
                      .as_object()
                      .if_contains("id"));
}
