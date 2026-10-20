// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/protocol.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace ais2api {
namespace {
[[noreturn]] void invalid(std::string message) {
    throw Error(400, "invalid_argument", std::move(message));
}
const json::value &at(const json::array &a, std::size_t i) {
    static const json::value nil;
    return i < a.size() ? a[i] : nil;
}
void put(json::array &a, std::size_t i, json::value value) {
    if (a.size() <= i)
        a.resize(i + 1);
    a[i] = std::move(value);
}
void trim(json::array &a) {
    while (!a.empty() && a.back().is_null())
        a.pop_back();
}
json::array single(json::value value) {
    json::array a;
    a.push_back(std::move(value));
    return a;
}
std::string str(const json::value &v) {
    return v.is_string() ? std::string(v.as_string()) : std::string{};
}
std::int64_t number(const json::value &v, std::int64_t fallback = 0) {
    if (v.is_int64())
        return v.as_int64();
    if (v.is_uint64() && v.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(v.as_uint64());
    return fallback;
}
const json::object &object(const json::value &v, std::string_view where) {
    if (!v.is_object())
        invalid(std::string(where) + " must be an object");
    return v.as_object();
}
const json::array &array(const json::value &v, std::string_view where) {
    if (!v.is_array())
        invalid(std::string(where) + " must be an array");
    return v.as_array();
}
std::string required_string(const json::object &o, std::string_view key) {
    auto p = o.if_contains(key);
    if (!p || !p->is_string() || p->as_string().empty())
        invalid(std::string(key) + " must be a nonempty string");
    return std::string(p->as_string());
}
const json::value &required_value(const json::object &o, std::string_view key) {
    auto p = o.if_contains(key);
    if (!p)
        invalid(std::string(key) + " is required");
    return *p;
}
void keys(const json::object &o, std::initializer_list<std::string_view> allowed,
          std::string_view where) {
    for (auto &kv : o)
        if (std::find(allowed.begin(), allowed.end(),
                      std::string_view(kv.key().data(), kv.key().size())) == allowed.end())
            invalid(std::string(where) + ": unsupported field " + std::string(kv.key()));
}
std::string model_name(std::string model, const Config &config) {
    if (auto p = config.model_aliases.if_contains(model)) {
        if (!p->is_string())
            invalid("Model alias must be a string");
        model = str(*p);
    }
    if (model.starts_with("models/"))
        model.erase(0, 7);
    if (model.empty() || model.find_first_of("/?#\\ \t\r\n") != std::string::npos)
        invalid("Invalid model name");
    return model;
}
json::value value_wire(const json::value &v);
json::array struct_wire(const json::object &o) {
    json::array fields;
    for (auto &kv : o)
        fields.push_back(json::array{kv.key(), value_wire(kv.value())});
    return fields;
}
json::value value_wire(const json::value &v) {
    if (v.is_null())
        return json::array{0};
    if (v.is_number())
        return json::array{nullptr, v};
    if (v.is_string())
        return json::array{nullptr, nullptr, v};
    if (v.is_bool())
        return json::array{nullptr, nullptr, nullptr, v};
    if (v.is_object())
        return json::array{nullptr, nullptr, nullptr, nullptr, single(struct_wire(v.as_object()))};
    json::array values;
    for (auto &x : v.as_array())
        values.push_back(value_wire(x));
    return json::array{nullptr, nullptr, nullptr, nullptr, nullptr, single(values)};
}
bool is_pair(const json::value &v) {
    return v.is_array() && v.as_array().size() == 2 && at(v.as_array(), 0).is_string();
}
json::value value_unwire(const json::value &v);
json::object struct_unwire(const json::value &v) {
    if (v.is_object())
        return v.as_object();
    if (!v.is_array())
        throw Error(502, "invalid_upstream", "Malformed function Struct");
    auto *a = &v.as_array();
    for (unsigned n = 0; n < 8 && a->size() == 1 && !is_pair((*a)[0]) && (*a)[0].is_array(); ++n)
        a = &(*a)[0].as_array();
    json::object out;
    for (auto &field : *a) {
        if (!is_pair(field))
            throw Error(502, "invalid_upstream", "Malformed function Struct entry");
        auto &p = field.as_array();
        out[str(p[0])] = value_unwire(p[1]);
    }
    return out;
}
json::value value_unwire(const json::value &v) {
    if (!v.is_array())
        return v;
    auto &a = v.as_array();
    if (!at(a, 0).is_null())
        return nullptr;
    if (!at(a, 1).is_null())
        return at(a, 1);
    if (!at(a, 2).is_null())
        return at(a, 2);
    if (!at(a, 3).is_null())
        return at(a, 3).is_bool() ? at(a, 3) : json::value(number(at(a, 3)) != 0);
    if (!at(a, 4).is_null())
        return struct_unwire(at(a, 4));
    if (!at(a, 5).is_null()) {
        if (!at(a, 5).is_array())
            throw Error(502, "invalid_upstream", "Malformed function ListValue");
        auto &container = at(a, 5).as_array();
        auto *items = &container;
        if (container.size() == 1 && container[0].is_array())
            items = &container[0].as_array();
        json::array out;
        for (auto &x : *items)
            out.push_back(value_unwire(x));
        return out;
    }
    return nullptr;
}
json::array encode_part(const json::object &p) {
    keys(p,
         {"text", "thought", "thoughtSignature", "inlineData", "fileData", "functionCall",
          "functionResponse", "executableCode", "codeExecutionResult"},
         "part");
    if (p.if_contains("fileData"))
        invalid("fileData and Files API are not supported");
    json::array out;
    unsigned payloads = 0;
    if (auto v = p.if_contains("text")) {
        if (!v->is_string())
            invalid("part.text must be a string");
        put(out, 1, *v);
        ++payloads;
    }
    if (auto v = p.if_contains("inlineData")) {
        auto &x = object(*v, "inlineData");
        keys(x, {"mimeType", "data"}, "inlineData");
        put(out, 2, json::array{required_string(x, "mimeType"), required_string(x, "data")});
        ++payloads;
    }
    for (auto [key, index] : {std::pair{"functionCall", 10}, std::pair{"functionResponse", 11}})
        if (auto v = p.if_contains(key)) {
            auto &x = object(*v, key);
            keys(x, {"name", "args", "response", "id"}, key);
            auto name = required_string(x, "name");
            auto data = x.if_contains(index == 10 ? "args" : "response");
            json::array fn{name, data ? json::value(single(struct_wire(object(*data, key))))
                                      : json::value(single(json::array{}))};
            if (auto id = x.if_contains("id")) {
                if (!id->is_string())
                    invalid("tool id must be a string");
                fn.push_back(*id);
            }
            put(out, index, std::move(fn));
            ++payloads;
        }
    if (auto v = p.if_contains("executableCode")) {
        auto &x = object(*v, "executableCode");
        keys(x, {"language", "code"}, "executableCode");
        auto language = string_or(x, "language", "PYTHON");
        if (language != "PYTHON" && language != "LANGUAGE_UNSPECIFIED")
            invalid("Unsupported executableCode language");
        put(out, 8, json::array{language == "PYTHON" ? 1 : 0, required_string(x, "code")});
        ++payloads;
    }
    if (auto v = p.if_contains("codeExecutionResult")) {
        auto &x = object(*v, "codeExecutionResult");
        keys(x, {"outcome", "output"}, "codeExecutionResult");
        auto output = x.if_contains("output");
        if (!output || !output->is_string())
            invalid("codeExecutionResult.output must be a string");
        static const std::map<std::string, int> outcomes{{"OUTCOME_UNSPECIFIED", 0},
                                                         {"OUTCOME_OK", 1},
                                                         {"OUTCOME_FAILED", 2},
                                                         {"OUTCOME_DEADLINE_EXCEEDED", 3}};
        auto outcome = outcomes.find(string_or(x, "outcome", "OUTCOME_UNSPECIFIED"));
        if (outcome == outcomes.end())
            invalid("Unsupported code execution outcome");
        put(out, 9, json::array{outcome->second, *output});
        ++payloads;
    }
    if (payloads != 1 && !(payloads == 0 && p.if_contains("thoughtSignature")))
        invalid("Each part must have one content payload or an opaque standalone thoughtSignature");
    if (auto v = p.if_contains("thought")) {
        if (!v->is_bool())
            invalid("thought must be boolean");
        if (v->as_bool())
            put(out, 12, 1);
    }
    if (auto v = p.if_contains("thoughtSignature")) {
        if (!v->is_string())
            invalid("thoughtSignature must be a string");
        put(out, 14, *v);
    }
    return out;
}
json::array content_wire(const json::object &c, bool system = false) {
    keys(c, {"role", "parts"}, "content");
    auto p = c.if_contains("parts");
    if (!p)
        invalid("content.parts is required");
    auto &parts = array(*p, "content.parts");
    if (parts.empty())
        invalid("content.parts cannot be empty");
    auto role = string_or(c, "role", system ? "system" : "user");
    if (role != "user" && role != "model" && !(system && role == "system"))
        invalid("Invalid content role");
    json::array encoded;
    for (auto &part : parts) {
        auto p = object(part, "part");
        json::array attachments;
        if (auto fr = p.if_contains("functionResponse"); fr && fr->is_object()) {
            if (auto extra = fr->as_object().if_contains("parts")) {
                attachments = array(*extra, "functionResponse.parts");
                fr->as_object().erase("parts");
            }
        }
        encoded.push_back(encode_part(p));
        for (auto &attachment : attachments) {
            auto &sub = object(attachment, "functionResponse part");
            if (!sub.if_contains("text") && !sub.if_contains("inlineData"))
                invalid("Function response attachments support text and inlineData only");
            encoded.push_back(encode_part(sub));
        }
    }
    return {encoded, role};
}
json::array schemas(const json::value &v, bool allow_null) {
    json::array out;
    for (auto &item : array(v, "schema union")) {
        auto &branch = object(item, "schema branch");
        auto type = string_or(branch, "type");
        if ((type == "null" || type == "NULL") && allow_null)
            continue;
        out.push_back(encode_schema(branch));
    }
    if (out.empty())
        invalid("schema union needs a non-null branch");
    return out;
}
void validate_generation(const json::object &g) {
    keys(g,
         {"stopSequences", "maxOutputTokens", "temperature", "topP", "topK", "responseMimeType",
          "responseSchema", "presencePenalty", "frequencyPenalty", "responseLogprobs", "logprobs",
          "responseModalities", "thinkingConfig", "mediaResolution", "imageConfig"},
         "generationConfig");
    for (auto key : {"temperature", "topP", "presencePenalty", "frequencyPenalty"})
        if (auto p = g.if_contains(key)) {
            if (!p->is_number())
                invalid(std::string(key) + " must be numeric");
        }
    for (auto key : {"maxOutputTokens", "topK", "logprobs"})
        if (auto p = g.if_contains(key)) {
            if (!(p->is_int64() || p->is_uint64()) || number(*p) < 0)
                invalid(std::string(key) + " must be a nonnegative integer");
        }
    if (auto p = g.if_contains("responseLogprobs"))
        if (!p->is_bool())
            invalid("responseLogprobs must be boolean");
    if (auto p = g.if_contains("responseSchema"))
        encode_schema(object(*p, "responseSchema"));
    if (auto p = g.if_contains("stopSequences"))
        for (auto &x : array(*p, "stopSequences"))
            if (!x.is_string())
                invalid("stopSequences must contain strings");
    if (auto p = g.if_contains("responseMimeType"))
        if (!p->is_string())
            invalid("responseMimeType must be a string");
}
json::array generation_wire(const json::object &g) {
    validate_generation(g);
    json::array a;
    for (auto [key, i] : {std::pair{"stopSequences", 1},
                          {"maxOutputTokens", 3},
                          {"temperature", 4},
                          {"topP", 5},
                          {"topK", 6},
                          {"responseMimeType", 7},
                          {"presencePenalty", 9},
                          {"frequencyPenalty", 10},
                          {"responseLogprobs", 11},
                          {"logprobs", 12}})
        if (auto p = g.if_contains(key))
            put(a, i, *p);
    if (auto p = g.if_contains("responseSchema"))
        put(a, 8, encode_schema(p->as_object()));
    if (auto p = g.if_contains("responseModalities")) {
        json::array modes;
        for (auto &v : array(*p, "responseModalities")) {
            auto s = str(v);
            if (s == "TEXT")
                modes.push_back(1);
            else if (s == "IMAGE")
                modes.push_back(2);
            else
                invalid("Unsupported response modality");
        }
        put(a, 14, modes);
    }
    if (auto p = g.if_contains("thinkingConfig")) {
        auto &t = object(*p, "thinkingConfig");
        keys(t, {"includeThoughts", "thinkingBudget", "thinkingLevel"}, "thinkingConfig");
        json::array t_wire{1, nullptr, nullptr, 3};
        if (t.if_contains("includeThoughts") || t.if_contains("thinkingBudget"))
            invalid("thinkingBudget/includeThoughts have no verified mapping in this private "
                    "upstream; use thinkingLevel");
        if (auto b = t.if_contains("thinkingLevel")) {
            static const std::map<std::string, int> m{
                {"LOW", 1}, {"MEDIUM", 2}, {"HIGH", 3}, {"MINIMAL", 4}};
            auto it = m.find(str(*b));
            if (it == m.end())
                invalid("Invalid thinkingLevel");
            put(t_wire, 3, it->second);
        }
        put(a, 16, t_wire);
    }
    if (auto p = g.if_contains("mediaResolution")) {
        static const std::map<std::string, int> m{{"MEDIA_RESOLUTION_LOW", 1},
                                                  {"MEDIA_RESOLUTION_MEDIUM", 2},
                                                  {"MEDIA_RESOLUTION_HIGH", 3}};
        auto it = m.find(str(*p));
        if (it == m.end())
            invalid("Invalid mediaResolution");
        put(a, 17, it->second);
    }
    if (auto p = g.if_contains("imageConfig")) {
        auto &img = object(*p, "imageConfig");
        keys(img, {"aspectRatio", "imageSize"}, "imageConfig");
        json::array x;
        if (auto q = img.if_contains("aspectRatio"))
            put(x, 0, *q);
        if (auto q = img.if_contains("imageSize"))
            put(x, 1, *q);
        put(a, 26, x);
    }
    return a;
}
void apply_extension(json::object &body, const json::object &ext) {
    keys(ext, {"generationConfig", "tools", "toolConfig", "safetySettings", "systemInstruction"},
         "ais2api");
    for (auto &kv : ext) {
        auto existing = body.if_contains(kv.key());
        if (existing && existing->is_object() && kv.value().is_object()) {
            auto &dest = existing->as_object();
            for (auto &sub : kv.value().as_object()) {
                if (dest.if_contains(sub.key()))
                    invalid("ais2api conflicts with standard field " + std::string(kv.key()) + "." +
                            std::string(sub.key()));
                dest[sub.key()] = sub.value();
            }
        } else {
            if (existing)
                invalid("ais2api conflicts with standard field " + std::string(kv.key()));
            body[kv.key()] = kv.value();
        }
    }
}
std::string finish_name(std::int64_t n) {
    static const char *names[] = {"FINISH_REASON_UNSPECIFIED",
                                  "STOP",
                                  "MAX_TOKENS",
                                  "SAFETY",
                                  "RECITATION",
                                  "LANGUAGE",
                                  "OTHER",
                                  "BLOCKLIST",
                                  "PROHIBITED_CONTENT",
                                  "SPII",
                                  "MALFORMED_FUNCTION_CALL",
                                  "IMAGE_SAFETY",
                                  "UNEXPECTED_TOOL_CALL",
                                  "TOO_MANY_TOOL_CALLS",
                                  "IMAGE_PROHIBITED_CONTENT",
                                  "NO_IMAGE",
                                  "IMAGE_RECITATION",
                                  "IMAGE_OTHER"};
    return n >= 0 && n < 18 ? names[n] : names[0];
}
json::object decode_part(const json::array &a) {
    json::object p;
    if (at(a, 1).is_string())
        p["text"] = a[1];
    bool thinking = (!at(a, 0).is_null() && (at(a, 0) == true || at(a, 0) == 1)) ||
                    at(a, 10) == true || at(a, 12) == 1;
    if (thinking)
        p["thought"] = true;
    if (at(a, 2).is_array()) {
        auto &x = a[2].as_array();
        if (at(x, 0).is_string() && at(x, 1).is_string())
            p["inlineData"] = json::object{{"mimeType", x[0]}, {"data", x[1]}};
    }
    for (auto [key, primary, fallback] :
         {std::tuple{"functionCall", 3, 10}, std::tuple{"functionResponse", 4, 11}}) {
        auto &v = at(a, primary).is_array() ? at(a, primary) : at(a, fallback);
        if (v.is_array() && at(v.as_array(), 0).is_string()) {
            auto &x = v.as_array();
            json::object f{{"name", x[0]},
                           {primary == 3 ? "args" : "response",
                            at(x, 1).is_null() ? json::object{} : struct_unwire(x[1])}};
            if (at(x, 2).is_string())
                f["id"] = x[2];
            p[key] = f;
        }
    }
    if (at(a, 8).is_string())
        p["executableCode"] = json::object{{"language", "PYTHON"}, {"code", a[8]}};
    else if (at(a, 8).is_array()) {
        auto &code = a[8].as_array();
        if (at(code, 1).is_string())
            p["executableCode"] = json::object{
                {"language", number(at(code, 0)) == 1 ? "PYTHON" : "LANGUAGE_UNSPECIFIED"},
                {"code", code[1]}};
    }
    if (at(a, 9).is_string())
        p["codeExecutionResult"] =
            json::object{{"outcome", "OUTCOME_UNSPECIFIED"}, {"output", a[9]}};
    else if (at(a, 9).is_array()) {
        auto &code = a[9].as_array();
        static const char *outcomes[] = {"OUTCOME_UNSPECIFIED", "OUTCOME_OK", "OUTCOME_FAILED",
                                         "OUTCOME_DEADLINE_EXCEEDED"};
        auto n = number(at(code, 0));
        if (at(code, 1).is_string())
            p["codeExecutionResult"] = json::object{
                {"outcome", n >= 0 && n < 4 ? outcomes[n] : outcomes[0]}, {"output", code[1]}};
    }
    if (at(a, 14).is_string())
        p["thoughtSignature"] = a[14];
    return p;
}
json::array tool_wire(const json::array &tools, const json::object *config) {
    std::string mode = "AUTO";
    std::set<std::string> allowed;
    if (config) {
        keys(*config, {"functionCallingConfig"}, "toolConfig");
        if (auto p = config->if_contains("functionCallingConfig")) {
            auto &f = object(*p, "functionCallingConfig");
            keys(f, {"mode", "allowedFunctionNames"}, "functionCallingConfig");
            mode = string_or(f, "mode", "AUTO");
            if (mode != "AUTO" && mode != "NONE" && mode != "ANY" && mode != "VALIDATED")
                invalid("Unsupported function calling mode");
            if (auto names = f.if_contains("allowedFunctionNames"))
                for (auto &n : array(*names, "allowedFunctionNames")) {
                    if (!n.is_string())
                        invalid("allowedFunctionNames must contain strings");
                    allowed.insert(str(n));
                }
        }
    }
    if (mode == "ANY" || mode == "VALIDATED")
        invalid(
            "Upstream has no verified forced/validated function-call control; use AUTO or NONE");
    if (mode == "NONE")
        return {};
    json::array out;
    for (auto &item : tools) {
        auto &t = object(item, "tool");
        keys(t,
             {"functionDeclarations", "codeExecution", "googleSearch", "googleSearchRetrieval",
              "googleMaps", "urlContext"},
             "tool");
        if (auto declarations = t.if_contains("functionDeclarations")) {
            json::array functions;
            for (auto &declaration : array(*declarations, "functionDeclarations")) {
                auto &f = object(declaration, "function declaration");
                keys(f, {"name", "description", "parameters", "parametersJsonSchema"},
                     "function declaration");
                auto name = required_string(f, "name");
                if (!allowed.empty() && !allowed.contains(name))
                    continue;
                if (f.if_contains("parameters") && f.if_contains("parametersJsonSchema"))
                    invalid("parameters conflicts with parametersJsonSchema");
                json::array entry{name, nullptr, nullptr};
                if (auto p = f.if_contains("description")) {
                    if (!p->is_string())
                        invalid("function description must be string");
                    entry[1] = *p;
                }
                auto p = f.if_contains("parameters");
                if (!p)
                    p = f.if_contains("parametersJsonSchema");
                if (p)
                    entry[2] = encode_schema(object(*p, "function parameters"));
                trim(entry);
                functions.push_back(entry);
            }
            if (!functions.empty())
                out.push_back(json::array{nullptr, functions});
        }
        for (auto key : {"codeExecution", "googleSearchRetrieval", "googleMaps", "urlContext"})
            if (auto p = t.if_contains(key))
                keys(object(*p, key), {}, key);
        if (t.if_contains("codeExecution"))
            out.push_back(single(json::array{}));
        if (auto search = t.if_contains("googleSearch")) {
            auto &options = object(*search, "googleSearch");
            keys(options, {"searchTypes"}, "googleSearch");
            bool web = true, image = false;
            if (auto types = options.if_contains("searchTypes")) {
                auto &types_object = object(*types, "searchTypes");
                keys(types_object, {"webSearch", "imageSearch"}, "searchTypes");
                web = types_object.if_contains("webSearch") != nullptr;
                image = types_object.if_contains("imageSearch") != nullptr;
                for (auto &type : types_object)
                    keys(object(type.value(), "search type"), {}, "search type");
            }
            if (image)
                out.push_back(json::parse(web ? "[null,null,null,[null,[[],[]]]]"
                                              : "[null,null,null,[null,[null,[]]]]")
                                  .as_array());
            else
                out.push_back(json::parse("[null,null,null,[null,[[]]]]").as_array());
        } else if (t.if_contains("googleSearchRetrieval"))
            out.push_back(json::parse("[null,null,null,[null,[[]]]]").as_array());
        if (t.if_contains("googleMaps")) {
            json::array a;
            put(a, 10, json::array{});
            out.push_back(a);
        }
        if (t.if_contains("urlContext")) {
            json::array a;
            put(a, 7, json::array{});
            out.push_back(a);
        }
    }
    if (mode == "ANY" && out.empty())
        invalid("No tools match tool choice");
    return out;
}
} // namespace

json::array encode_contents(const json::array &contents) {
    if (contents.empty())
        invalid("contents cannot be empty");
    json::array out;
    for (auto &c : contents)
        out.push_back(content_wire(object(c, "content")));
    return out;
}

json::array encode_schema(const json::object &s) {
    keys(s, {"type",     "format",     "description",     "nullable",      "enum",
             "items",    "properties", "required",        "minProperties", "maxProperties",
             "minimum",  "maximum",    "minLength",       "maxLength",     "pattern",
             "example",  "oneOf",      "anyOf",           "allOf",         "not",
             "maxItems", "minItems",   "propertyOrdering"},
         "schema");
    static const std::map<std::string, int> types{{"string", 1},  {"number", 2}, {"integer", 3},
                                                  {"boolean", 4}, {"array", 5},  {"object", 6}};
    for (auto k : {"format", "description", "pattern"})
        if (auto p = s.if_contains(k); p && !p->is_string())
            invalid(std::string("schema.") + k + " must be a string");
    if (auto p = s.if_contains("nullable"); p && !p->is_bool())
        invalid("schema.nullable must be boolean");
    for (auto k :
         {"minProperties", "maxProperties", "minLength", "maxLength", "maxItems", "minItems"})
        if (auto p = s.if_contains(k); p && (!(p->is_int64() || p->is_uint64()) || number(*p) < 0))
            invalid(std::string("schema.") + k + " must be a nonnegative integer");
    for (auto k : {"minimum", "maximum"})
        if (auto p = s.if_contains(k); p && !p->is_number())
            invalid(std::string("schema.") + k + " must be numeric");
    for (auto k : {"enum", "required", "propertyOrdering"})
        if (auto p = s.if_contains(k))
            for (auto &v : array(*p, k))
                if (!v.is_string())
                    invalid(std::string("schema.") + k + " must contain strings");
    std::string type = string_or(s, "type");
    bool nullable = false;
    for (auto k : {"oneOf", "anyOf"})
        if (auto p = s.if_contains(k))
            for (auto &branch : array(*p, "schema union")) {
                auto t = string_or(object(branch, "schema branch"), "type");
                if (t == "null" || t == "NULL")
                    nullable = true;
            }
    if (auto p = s.if_contains("type"); p && p->is_array()) {
        for (auto &v : p->as_array()) {
            auto t = str(v);
            if (t == "null" || t == "NULL")
                nullable = true;
            else if (type.empty())
                type = t;
            else
                invalid("Only nullable single-type arrays are supported");
        }
    }
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (type.empty()) {
        if (s.if_contains("properties"))
            type = "object";
        else if (s.if_contains("items"))
            type = "array";
        else
            for (auto k : {"oneOf", "anyOf", "allOf"})
                if (auto p = s.if_contains(k); p && p->is_array())
                    for (auto &v : p->as_array()) {
                        auto &branch = object(v, "schema branch");
                        auto candidate = string_or(branch, "type");
                        if (candidate == "null" || candidate == "NULL") {
                            nullable = true;
                            continue;
                        }
                        if (type.empty()) {
                            if (!candidate.empty())
                                type = candidate;
                            else if (branch.if_contains("properties"))
                                type = "object";
                            else if (branch.if_contains("items"))
                                type = "array";
                        }
                    }
    }
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto it = types.find(type);
    if (it == types.end())
        invalid("Schema requires a supported concrete type");
    json::array out{it->second};
    for (auto [k, i] : {std::pair{"format", 1},
                        {"description", 2},
                        {"nullable", 3},
                        {"enum", 4},
                        {"required", 7},
                        {"minProperties", 8},
                        {"maxProperties", 9},
                        {"minimum", 10},
                        {"maximum", 11},
                        {"minLength", 12},
                        {"maxLength", 13},
                        {"pattern", 14},
                        {"maxItems", 20},
                        {"minItems", 21},
                        {"propertyOrdering", 22}})
        if (auto p = s.if_contains(k))
            put(out, i, *p);
    if (nullable)
        put(out, 3, true);
    if (auto p = s.if_contains("items"))
        put(out, 5, encode_schema(object(*p, "schema.items")));
    if (auto p = s.if_contains("properties")) {
        json::array props;
        for (auto &kv : object(*p, "schema.properties"))
            props.push_back(
                json::array{kv.key(), encode_schema(object(kv.value(), "property schema"))});
        put(out, 6, props);
    }
    if (auto p = s.if_contains("example"))
        put(out, 15, value_wire(*p));
    for (auto [k, i] : {std::pair{"oneOf", 16}, {"anyOf", 17}, {"allOf", 18}})
        if (auto p = s.if_contains(k))
            put(out, i, schemas(*p, i != 18));
    if (auto p = s.if_contains("not"))
        put(out, 19, encode_schema(object(*p, "schema.not")));
    trim(out);
    return out;
}

Request normalize_gemini(std::string model, const json::object &input, bool stream,
                         const Config &config) {
    keys(input,
         {"contents", "systemInstruction", "generationConfig", "tools", "toolConfig",
          "safetySettings", "ais2api"},
         "Gemini request");
    Request r;
    r.model = model_name(std::move(model), config);
    r.stream = stream;
    r.gemini = input;
    if (auto p = r.gemini.if_contains("ais2api")) {
        auto envelope = object(*p, "ais2api");
        keys(envelope, {"gemini"}, "ais2api");
        r.gemini.erase("ais2api");
        if (auto ext = envelope.if_contains("gemini"))
            apply_extension(r.gemini, object(*ext, "ais2api.gemini"));
    }
    if (auto d = config.model_defaults.if_contains(r.model)) {
        for (auto &kv : object(*d, "model defaults"))
            if (!r.gemini.if_contains(kv.key()))
                r.gemini[kv.key()] = kv.value();
    }
    auto contents = r.gemini.if_contains("contents");
    if (!contents)
        invalid("contents is required");
    encode_contents(array(*contents, "contents"));
    if (auto p = r.gemini.if_contains("systemInstruction"))
        content_wire(object(*p, "systemInstruction"), true);
    if (auto p = r.gemini.if_contains("generationConfig"))
        generation_wire(object(*p, "generationConfig"));
    const json::object *tc = nullptr;
    if (auto p = r.gemini.if_contains("toolConfig"))
        tc = &object(*p, "toolConfig");
    if (auto p = r.gemini.if_contains("tools"))
        tool_wire(array(*p, "tools"), tc);
    else if (tc)
        tool_wire({}, tc);
    return r;
}

json::array encode_request(const Request &r, const json::array &base, std::string_view snapshot) {
    json::array out = base;
    put(out, 0, json::value("models/" + r.model));
    auto c = r.gemini.if_contains("contents");
    if (!c)
        invalid("contents is required");
    put(out, 1, encode_contents(array(*c, "contents")));
    put(out, 4, json::value(snapshot));
    put(out, 7, json::value("models/" + r.model));
    put(out, 10, 1);
    for (auto i : {2, 3, 5, 6, 11})
        put(out, i, nullptr);
    if (auto p = r.gemini.if_contains("generationConfig"))
        put(out, 3, generation_wire(object(*p, "generationConfig")));
    if (auto p = r.gemini.if_contains("systemInstruction"))
        put(out, 5, content_wire(object(*p, "systemInstruction"), true));
    const json::object *tc = nullptr;
    if (auto p = r.gemini.if_contains("toolConfig"))
        tc = &object(*p, "toolConfig");
    if (auto p = r.gemini.if_contains("tools")) {
        auto tools = tool_wire(array(*p, "tools"), tc);
        if (!tools.empty())
            put(out, 6, tools);
    }
    if (auto p = r.gemini.if_contains("safetySettings")) {
        static const std::map<std::string, int> categories{{"HARM_CATEGORY_HARASSMENT", 7},
                                                           {"HARM_CATEGORY_HATE_SPEECH", 8},
                                                           {"HARM_CATEGORY_SEXUALLY_EXPLICIT", 9},
                                                           {"HARM_CATEGORY_DANGEROUS_CONTENT", 10}};
        static const std::map<std::string, int> thresholds{{"BLOCK_LOW_AND_ABOVE", 1},
                                                           {"BLOCK_MEDIUM_AND_ABOVE", 2},
                                                           {"BLOCK_ONLY_HIGH", 3},
                                                           {"BLOCK_NONE", 4},
                                                           {"OFF", 5}};
        json::array settings;
        for (auto &setting : array(*p, "safetySettings")) {
            auto &s = object(setting, "safety setting");
            keys(s, {"category", "threshold"}, "safety setting");
            auto ci = categories.find(string_or(s, "category"));
            auto ti = thresholds.find(string_or(s, "threshold"));
            if (ci == categories.end() || ti == thresholds.end())
                invalid("Unsupported safety setting");
            settings.push_back(json::array{nullptr, nullptr, ci->second, ti->second});
        }
        put(out, 2, settings);
    }
    trim(out);
    return out;
}

json::object decode_chunk(const json::value &wire) {
    if (!wire.is_array())
        throw Error(502, "invalid_upstream", "Wire chunk must be an array");
    auto &a = wire.as_array();
    json::object out;
    if (at(a, 0).is_array()) {
        json::array candidates;
        std::size_t index = 0;
        for (auto &v : a[0].as_array()) {
            if (!v.is_array())
                continue;
            auto &c = v.as_array();
            json::object candidate{{"index", index++}};
            if (at(c, 0).is_array()) {
                auto &content = c[0].as_array();
                json::array parts;
                if (at(content, 0).is_array())
                    for (auto &part : content[0].as_array())
                        if (part.is_array()) {
                            auto p = decode_part(part.as_array());
                            if (!p.empty())
                                parts.push_back(p);
                        }
                candidate["content"] = json::object{
                    {"role", str(at(content, 1)).empty() ? "model" : str(at(content, 1))},
                    {"parts", parts}};
            }
            if (at(c, 1).is_number())
                candidate["finishReason"] = finish_name(number(c[1]));
            if (at(c, 3).is_string())
                candidate["finishMessage"] = c[3];
            if (at(c, 4).is_array() && !c[4].as_array().empty())
                candidate["ais2api"] = json::object{{"wireSafetyRatings", c[4]}};
            candidates.push_back(candidate);
        }
        if (!candidates.empty())
            out["candidates"] = candidates;
    }
    if (at(a, 2).is_array()) {
        auto &u = a[2].as_array();
        auto prompt = number(at(u, 0)), visible = number(at(u, 1)), thoughts = number(at(u, 9));
        if (prompt < 0 || visible < 0 || thoughts < 0 || visible > INT64_MAX - thoughts ||
            prompt > INT64_MAX - visible - thoughts)
            throw Error(502, "invalid_upstream", "Invalid token usage counts");
        out["usageMetadata"] =
            json::object{{"promptTokenCount", prompt},
                         {"candidatesTokenCount", visible + thoughts},
                         {"totalTokenCount", number(at(u, 2), prompt + visible + thoughts)},
                         {"cachedContentTokenCount", number(at(u, 3))},
                         {"thoughtsTokenCount", thoughts}};
    }
    if (at(a, 7).is_string())
        out["responseId"] = a[7];
    return out;
}

bool has_content(const json::object &event) {
    auto c = event.if_contains("candidates");
    if (!c || !c->is_array())
        return false;
    for (auto &v : c->as_array())
        if (v.is_object())
            if (auto content = v.as_object().if_contains("content");
                content && content->is_object())
                if (auto parts = content->as_object().if_contains("parts");
                    parts && parts->is_array())
                    for (auto &part : parts->as_array())
                        if (part.is_object()) {
                            auto &p = part.as_object();
                            if (auto t = p.if_contains("text");
                                t && t->is_string() && !t->as_string().empty())
                                return true;
                            for (auto key : {"inlineData", "functionCall", "functionResponse",
                                             "executableCode", "codeExecutionResult"})
                                if (p.if_contains(key))
                                    return true;
                        }
    return false;
}

void merge_event(json::object &aggregate, const json::object &event, std::size_t limit) {
    // Construct transactionally: an oversized event cannot partially alter the result.
    json::object next = aggregate;
    for (auto &kv : event)
        if (kv.key() != "candidates")
            next[kv.key()] = kv.value();
    if (auto incoming = event.if_contains("candidates"); incoming && incoming->is_array()) {
        if (!next.if_contains("candidates"))
            next["candidates"] = json::array{};
        auto &dest = next["candidates"].as_array();
        for (auto &cv : incoming->as_array()) {
            auto &c = object(cv, "candidate");
            auto idx = c.if_contains("index") ? number(c.at("index")) : 0;
            json::object *target = nullptr;
            for (auto &existing : dest)
                if (existing.is_object()) {
                    auto old_index = existing.as_object().if_contains("index");
                    if ((old_index ? number(*old_index) : 0) == idx) {
                        target = &existing.as_object();
                        break;
                    }
                }
            if (!target) {
                dest.push_back(json::object{{"index", idx}});
                target = &dest.back().as_object();
            }
            for (auto &ck : c)
                if (ck.key() != "content")
                    (*target)[ck.key()] = ck.value();
            if (auto content = c.if_contains("content"); content && content->is_object()) {
                if (!target->if_contains("content"))
                    (*target)["content"] =
                        json::object{{"role", "model"}, {"parts", json::array{}}};
                auto &dc = (*target)["content"].as_object();
                auto &src = content->as_object();
                if (auto role = src.if_contains("role"))
                    dc["role"] = *role;
                if (auto ps = src.if_contains("parts"); ps && ps->is_array()) {
                    auto &parts = dc["parts"].as_array();
                    for (auto &part : ps->as_array()) {
                        bool joined = false;
                        if (part.is_object() && !parts.empty() && parts.back().is_object()) {
                            auto &a = parts.back().as_object();
                            auto &b = part.as_object();
                            if (a.if_contains("text") && b.if_contains("text") &&
                                bool_or(a, "thought") == bool_or(b, "thought") && a.size() <= 2 &&
                                b.size() <= 2 && a.at("text").is_string() &&
                                b.at("text").is_string()) {
                                a["text"] = str(a.at("text")) + str(b.at("text"));
                                joined = true;
                            }
                        }
                        if (!joined)
                            parts.push_back(part);
                    }
                }
            }
        }
    }
    if (json::serialize(next).size() > limit)
        throw Error(413, "response_too_large",
                    "Generated response exceeds configured object limit");
    aggregate = std::move(next);
}

namespace {
json::array message_parts(const json::value &content) {
    if (content.is_null())
        return {};
    if (content.is_string())
        return json::array{json::object{{"text", content}}};
    json::array parts;
    for (auto &value : array(content, "message content")) {
        auto &c = object(value, "content part");
        auto type = required_string(c, "type");
        if (type == "text") {
            keys(c, {"type", "text"}, "text content");
            auto p = c.if_contains("text");
            if (!p || !p->is_string())
                invalid("text content requires text string");
            parts.push_back(json::object{{"text", *p}});
        } else if (type == "image_url") {
            keys(c, {"type", "image_url"}, "image content");
            auto p = c.if_contains("image_url");
            if (!p)
                invalid("image_url is required");
            auto &image = object(*p, "image_url");
            keys(image, {"url", "detail"}, "image_url");
            auto url = required_string(image, "url");
            auto semi = url.find(";base64,");
            if (!url.starts_with("data:image/") || semi == std::string::npos ||
                semi + 8 >= url.size())
                invalid("image_url must be an image data URL; remote URLs require the server media "
                        "loader");
            parts.push_back(
                json::object{{"inlineData", json::object{{"mimeType", url.substr(5, semi - 5)},
                                                         {"data", url.substr(semi + 8)}}}});
        } else if (type == "input_audio") {
            keys(c, {"type", "input_audio"}, "audio content");
            auto p = c.if_contains("input_audio");
            if (!p)
                invalid("input_audio is required");
            auto &audio = object(*p, "input_audio");
            keys(audio, {"data", "format"}, "input_audio");
            auto format = required_string(audio, "format");
            if (format != "wav" && format != "mp3")
                invalid("input_audio format must be wav or mp3");
            parts.push_back(json::object{
                {"inlineData",
                 json::object{{"mimeType", format == "wav" ? "audio/wav" : "audio/mpeg"},
                              {"data", required_string(audio, "data")}}}});
        } else
            invalid("Unsupported OpenAI content part type: " + type);
    }
    return parts;
}
json::object args_json(const json::value &value) {
    if (!value.is_string())
        invalid("Tool arguments must be a JSON string");
    boost::system::error_code ec;
    auto v = json::parse(value.as_string(), ec);
    if (ec || !v.is_object())
        invalid("Tool arguments must encode a JSON object");
    return v.as_object();
}
void tool_choice(json::object &body, const json::value &choice) {
    std::string mode;
    json::array allowed;
    if (choice.is_string()) {
        auto s = str(choice);
        if (s == "auto")
            mode = "AUTO";
        else if (s == "none")
            mode = "NONE";
        else if (s == "required")
            mode = "ANY";
        else
            invalid("Unsupported tool_choice");
    } else {
        auto &c = object(choice, "tool_choice");
        keys(c, {"type", "function"}, "tool_choice");
        if (string_or(c, "type") != "function" || !c.if_contains("function"))
            invalid("tool_choice must select a function");
        auto &f = object(c.at("function"), "tool_choice.function");
        allowed.push_back(json::value(required_string(f, "name")));
        mode = "ANY";
    }
    json::object config{{"mode", mode}};
    if (!allowed.empty())
        config["allowedFunctionNames"] = allowed;
    body["toolConfig"] = json::object{{"functionCallingConfig", config}};
}
json::object usage_openai(const json::object &u) {
    auto get = [&](std::string_view k) {
        auto p = u.if_contains(k);
        return p ? number(*p) : 0;
    };
    return {
        {"prompt_tokens", get("promptTokenCount")},
        {"completion_tokens", get("candidatesTokenCount")},
        {"total_tokens", get("totalTokenCount")},
        {"prompt_tokens_details", json::object{{"cached_tokens", get("cachedContentTokenCount")}}},
        {"completion_tokens_details",
         json::object{{"reasoning_tokens", get("thoughtsTokenCount")}}}};
}
std::string finish_openai(std::string reason, bool calls) {
    if (reason == "MAX_TOKENS")
        return "length";
    if (reason == "SAFETY" || reason == "RECITATION" || reason == "BLOCKLIST" ||
        reason == "PROHIBITED_CONTENT" || reason == "SPII" || reason == "IMAGE_SAFETY" ||
        reason == "IMAGE_PROHIBITED_CONTENT" || reason == "IMAGE_RECITATION")
        return "content_filter";
    return calls ? "tool_calls" : "stop";
}
json::object openai_message(const json::object &content, Store *store, std::string_view principal,
                            std::string_view response_id, bool delta,
                            json::object *native_output = nullptr) {
    json::object message{{"role", "assistant"}};
    auto native_content = content;
    std::string registry = "stream:" + std::string(response_id);
    json::object indices =
        store ? store->get_tool(principal, registry).value_or(json::object{}) : json::object{};
    std::string text;
    json::array calls;
    auto p = content.if_contains("parts");
    bool rich = false;
    std::size_t part_index = 0;
    if (p && p->is_array())
        for (auto &value : p->as_array()) {
            auto &part = object(value, "response part");
            if (auto t = part.if_contains("text"); t && t->is_string() && !bool_or(part, "thought"))
                text += str(*t);
            if (part.if_contains("thoughtSignature") || bool_or(part, "thought") ||
                part.if_contains("inlineData") || part.if_contains("executableCode") ||
                part.if_contains("codeExecutionResult") || part.if_contains("functionResponse"))
                rich = true;
            if (auto f = part.if_contains("functionCall")) {
                auto fn = object(*f, "functionCall");
                auto id = string_or(fn, "id");
                std::size_t index = calls.size();
                if (id.empty()) {
                    const auto ordinal = store ? indices.size() : calls.size();
                    id = "call_" + sha256(std::string(response_id) + ":" + json::serialize(fn) +
                                          ":" + std::to_string(ordinal))
                                       .substr(0, 24);
                    native_content.at("parts")
                        .as_array()[part_index]
                        .as_object()
                        .at("functionCall")
                        .as_object()["id"] = id;
                }
                if (store) {
                    if (auto found = indices.if_contains(id))
                        index = static_cast<std::size_t>(number(*found));
                    else {
                        if (indices.size() >= 4096)
                            throw Error(413, "tool_registry_limit",
                                        "Response contains too many distinct tool calls");
                        index = indices.size();
                        indices[id] = index;
                        store->put_tool(principal, registry, indices);
                    }
                    json::object metadata = store->get_tool(principal, id).value_or(json::object{});
                    metadata["name"] = string_or(fn, "name");
                    metadata["response_id"] = response_id;
                    if (auto args = fn.if_contains("args"))
                        metadata["args"] = *args;
                    if (auto sig = part.if_contains("thoughtSignature"))
                        metadata["thoughtSignature"] = *sig;
                    store->put_tool(principal, id, metadata);
                }
                json::object call{
                    {"id", id},
                    {"type", "function"},
                    {"function",
                     json::object{
                         {"name", string_or(fn, "name")},
                         {"arguments",
                          json::serialize(fn.if_contains("args") ? fn.at("args")
                                                                 : json::value(json::object{}))}}}};
                if (delta)
                    call["index"] = index;
                calls.push_back(call);
            }
            ++part_index;
        }
    if (!text.empty())
        message["content"] = text;
    else if (!delta)
        message["content"] = nullptr;
    if (!calls.empty())
        message["tool_calls"] = calls;
    if (rich && !delta)
        message["ais2api"] = json::object{{"gemini_content", native_content}};
    if (native_output)
        *native_output = std::move(native_content);
    return message;
}
} // namespace

Request normalize_openai(const json::object &input, const Config &config, Store *store,
                         std::string principal) {
    keys(input,
         {"model",
          "messages",
          "stream",
          "stream_options",
          "temperature",
          "top_p",
          "max_tokens",
          "max_completion_tokens",
          "stop",
          "presence_penalty",
          "frequency_penalty",
          "logprobs",
          "top_logprobs",
          "tools",
          "tool_choice",
          "parallel_tool_calls",
          "response_format",
          "reasoning_effort",
          "n",
          "user",
          "metadata",
          "store",
          "ais2api"},
         "OpenAI request");
    auto model = model_name(required_string(input, "model"), config);
    auto m = input.if_contains("messages");
    if (!m)
        invalid("messages is required");
    if (auto n = input.if_contains("n"); n && number(*n) != 1)
        invalid("Only n=1 is supported");
    if (auto v = input.if_contains("store"); v && (!v->is_bool() || v->as_bool()))
        invalid("OpenAI store is not supported");
    if (auto v = input.if_contains("stream"); v && !v->is_bool())
        invalid("stream must be boolean");
    if (auto v = input.if_contains("parallel_tool_calls"); v && !v->is_bool())
        invalid("parallel_tool_calls must be boolean");
    json::array contents, system;
    std::map<std::string, std::string> call_names;
    for (auto &message : array(*m, "messages")) {
        auto &msg = object(message, "message");
        keys(msg, {"role", "content", "name", "tool_calls", "tool_call_id", "refusal", "ais2api"},
             "message");
        auto role = required_string(msg, "role");
        if (role != "system" && role != "developer" && role != "user" && role != "assistant" &&
            role != "tool")
            invalid("Unsupported message role");
        auto c = msg.if_contains("content");
        auto parts = c ? message_parts(*c) : json::array{};
        bool native = false;
        if (auto ext = msg.if_contains("ais2api")) {
            auto &e = object(*ext, "message.ais2api");
            keys(e, {"gemini_content"}, "message.ais2api");
            auto p = e.if_contains("gemini_content");
            if (!p)
                invalid("message.ais2api.gemini_content is required");
            if (role != "assistant")
                invalid("gemini_content is only valid on assistant messages");
            auto &gc = object(*p, "gemini_content");
            content_wire(gc);
            if (string_or(gc, "role", "model") != "model")
                invalid("assistant gemini_content.role must be model");
            parts = array(gc.at("parts"), "gemini_content.parts");
            native = true;
        }
        if (auto tc = msg.if_contains("tool_calls")) {
            if (role != "assistant")
                invalid("tool_calls requires assistant role");
            for (auto &value : array(*tc, "tool_calls")) {
                auto &call = object(value, "tool call");
                keys(call, {"id", "type", "function"}, "tool call");
                if (string_or(call, "type") != "function")
                    invalid("Only function tools are supported");
                auto id = required_string(call, "id");
                auto &f = object(required_value(call, "function"), "tool function");
                keys(f, {"name", "arguments"}, "tool function");
                auto name = required_string(f, "name");
                auto arg = f.if_contains("arguments");
                if (!arg)
                    invalid("Tool arguments are required");
                auto args = args_json(*arg);
                call_names[id] = name;
                json::object part{
                    {"functionCall", json::object{{"name", name}, {"args", args}, {"id", id}}}};
                if (store)
                    if (auto metadata = store->get_tool(principal, id)) {
                        if (auto old_name = metadata->if_contains("name");
                            old_name && str(*old_name) != name)
                            invalid("tool_call_id conflicts with its recorded function name");
                        if (auto old_args = metadata->if_contains("args");
                            old_args && *old_args != args)
                            invalid("tool_call_id conflicts with its recorded function arguments");
                        if (auto sig = metadata->if_contains("thoughtSignature"))
                            part["thoughtSignature"] = *sig;
                    }
                if (native) {
                    bool match = false;
                    for (auto &np : parts)
                        if (np.is_object())
                            if (auto nf = np.as_object().if_contains("functionCall");
                                nf && nf->is_object() && string_or(nf->as_object(), "id") == id) {
                                if (string_or(nf->as_object(), "name") != name ||
                                    !nf->as_object().if_contains("args") ||
                                    nf->as_object().at("args") != args)
                                    invalid("gemini_content conflicts with tool_calls");
                                match = true;
                            }
                    if (!match)
                        invalid("gemini_content is missing standard tool call");
                } else
                    parts.push_back(part);
            }
        }
        if (native && c && !c->is_null()) {
            std::string native_text;
            for (auto &p : parts)
                if (p.is_object() && !bool_or(p.as_object(), "thought"))
                    native_text += string_or(p.as_object(), "text");
            if (!c->is_string() || str(*c) != native_text)
                invalid("gemini_content conflicts with standard content");
        }
        if (role == "tool") {
            auto id = required_string(msg, "tool_call_id");
            auto name = call_names[id];
            if (name.empty() && store)
                if (auto metadata = store->get_tool(principal, id))
                    name = string_or(*metadata, "name");
            if (name.empty())
                invalid("Unknown tool_call_id");
            if (!c || !c->is_string())
                invalid("Tool result content must be a string");
            boost::system::error_code ec;
            auto result = json::parse(c->as_string(), ec);
            if (ec || !result.is_object())
                result = json::object{{"result", *c}};
            parts = json::array{
                json::object{{"functionResponse",
                              json::object{{"name", name}, {"response", result}, {"id", id}}}}};
        }
        if (role == "system" || role == "developer") {
            for (auto &p : parts)
                system.push_back(p);
            continue;
        }
        if (parts.empty())
            invalid("Message has no supported content");
        contents.push_back(
            json::object{{"role", role == "assistant" ? "model" : "user"}, {"parts", parts}});
    }
    json::object body{{"contents", contents}}, gen;
    if (!system.empty())
        body["systemInstruction"] = json::object{{"parts", system}};
    for (auto [source, target] : {std::pair{"temperature", "temperature"},
                                  {"top_p", "topP"},
                                  {"presence_penalty", "presencePenalty"},
                                  {"frequency_penalty", "frequencyPenalty"},
                                  {"logprobs", "responseLogprobs"},
                                  {"top_logprobs", "logprobs"}})
        if (auto p = input.if_contains(source))
            gen[target] = *p;
    if (input.if_contains("max_tokens") && input.if_contains("max_completion_tokens"))
        invalid("max_tokens conflicts with max_completion_tokens");
    if (auto p = input.if_contains("max_tokens"))
        gen["maxOutputTokens"] = *p;
    if (auto p = input.if_contains("max_completion_tokens"))
        gen["maxOutputTokens"] = *p;
    if (auto p = input.if_contains("stop")) {
        if (p->is_string())
            gen["stopSequences"] = json::array{*p};
        else
            gen["stopSequences"] = array(*p, "stop");
    }
    if (auto p = input.if_contains("reasoning_effort")) {
        auto level = str(*p);
        std::transform(level.begin(), level.end(), level.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        gen["thinkingConfig"] = json::object{{"thinkingLevel", level}};
    }
    if (auto p = input.if_contains("response_format")) {
        auto &f = object(*p, "response_format");
        keys(f, {"type", "json_schema"}, "response_format");
        auto type = required_string(f, "type");
        if (type == "json_object")
            gen["responseMimeType"] = "application/json";
        else if (type == "json_schema") {
            auto q = f.if_contains("json_schema");
            if (!q)
                invalid("json_schema is required");
            auto &schema = object(*q, "json_schema");
            keys(schema, {"name", "description", "strict", "schema"}, "json_schema");
            if (bool_or(schema, "strict"))
                invalid("strict JSON Schema is not supported by this upstream");
            auto s = schema.if_contains("schema");
            if (!s)
                invalid("json_schema.schema is required");
            gen["responseMimeType"] = "application/json";
            gen["responseSchema"] = *s;
        } else if (type != "text")
            invalid("Unsupported response_format");
    }
    if (!gen.empty())
        body["generationConfig"] = gen;
    if (auto p = input.if_contains("tools")) {
        json::array functions;
        for (auto &v : array(*p, "tools")) {
            auto &t = object(v, "tool");
            keys(t, {"type", "function"}, "tool");
            if (string_or(t, "type") != "function")
                invalid("Only function tools are supported in standard OpenAI tools");
            auto f = object(required_value(t, "function"), "function");
            if (auto strict = f.if_contains("strict")) {
                if (!strict->is_bool() || strict->as_bool())
                    invalid("strict function schema is not supported");
                f.erase("strict");
            }
            functions.push_back(f);
        }
        body["tools"] = json::array{json::object{{"functionDeclarations", functions}}};
    }
    if (auto p = input.if_contains("tool_choice"))
        tool_choice(body, *p);
    if (auto p = input.if_contains("ais2api")) {
        auto &ext = object(*p, "ais2api");
        keys(ext, {"gemini"}, "ais2api");
        if (auto g = ext.if_contains("gemini"))
            apply_extension(body, object(*g, "ais2api.gemini"));
    }
    auto request = normalize_gemini(model, body, bool_or(input, "stream"), config);
    request.protocol = Protocol::openai;
    request.principal = std::move(principal);
    if (auto p = input.if_contains("stream_options")) {
        auto &options = object(*p, "stream_options");
        keys(options, {"include_usage"}, "stream_options");
        if (auto include = options.if_contains("include_usage"); include && !include->is_bool())
            invalid("include_usage must be boolean");
        request.include_usage = bool_or(options, "include_usage");
    }
    return request;
}

json::object openai_response(const json::object &gemini, std::string_view model,
                             std::string_view id, Store *store, std::string_view principal) {
    json::array choices;
    if (auto cs = gemini.if_contains("candidates"); cs && cs->is_array())
        for (auto &cv : cs->as_array()) {
            auto &c = object(cv, "candidate");
            auto content = c.if_contains("content");
            auto message = openai_message(content ? object(*content, "content") : json::object{},
                                          store, principal, id, false);
            auto reason = finish_openai(string_or(c, "finishReason", "STOP"),
                                        message.if_contains("tool_calls") != nullptr);
            choices.push_back(
                json::object{{"index", c.if_contains("index") ? c.at("index") : json::value(0)},
                             {"message", message},
                             {"finish_reason", reason},
                             {"logprobs", nullptr}});
        }
    json::object result{{"id", id},
                        {"object", "chat.completion"},
                        {"created", unix_seconds()},
                        {"model", model},
                        {"choices", choices}};
    if (auto u = gemini.if_contains("usageMetadata"))
        result["usage"] = usage_openai(object(*u, "usageMetadata"));
    return result;
}

json::object openai_delta(const json::object &gemini, std::string_view model, std::string_view id,
                          Store *store, std::string_view principal) {
    json::array choices;
    auto native_gemini = gemini;
    if (auto cs = native_gemini.if_contains("candidates"); cs && cs->is_array())
        for (auto &cv : cs->as_array()) {
            if (!cv.is_object())
                invalid("candidate must be an object");
            auto &c = cv.as_object();
            auto content = c.if_contains("content");
            json::object native_content;
            auto delta = openai_message(content ? object(*content, "content") : json::object{},
                                        store, principal, id, true, &native_content);
            if (content)
                c["content"] = std::move(native_content);
            json::value reason;
            if (auto f = c.if_contains("finishReason")) {
                bool called = delta.if_contains("tool_calls") != nullptr;
                if (!called && store)
                    if (auto state = store->get_tool(principal, "stream:" + std::string(id)))
                        called = !state->empty();
                reason = finish_openai(str(*f), called);
            }
            choices.push_back(
                json::object{{"index", c.if_contains("index") ? c.at("index") : json::value(0)},
                             {"delta", delta},
                             {"finish_reason", reason},
                             {"logprobs", nullptr}});
        }
    for (auto &choice : choices)
        choice.as_object().at("delta").as_object()["ais2api"] =
            json::object{{"gemini_delta", native_gemini}};
    json::object result{{"id", id},
                        {"object", "chat.completion.chunk"},
                        {"created", unix_seconds()},
                        {"model", model},
                        {"choices", choices}};
    if (auto u = gemini.if_contains("usageMetadata"))
        result["usage"] = usage_openai(object(*u, "usageMetadata"));
    if (choices.empty())
        result["ais2api"] = json::object{{"gemini_delta", gemini}};
    return result;
}

WireStreamParser::WireStreamParser(std::size_t max_object_bytes) : limit_(max_object_bytes) {
    if (limit_ == 0)
        invalid("Wire parser limit must be positive");
}
void WireStreamParser::feed(std::string_view bytes, const std::function<void(json::value)> &emit) {
    for (char ch : bytes) {
        if (!prefix_done_) {
            if (preamble_.empty() && std::isspace(static_cast<unsigned char>(ch)))
                continue;
            if (ch == '[' && preamble_.empty())
                prefix_done_ = true;
            else {
                preamble_.push_back(ch);
                constexpr std::string_view prefix = ")]}'";
                if (preamble_.size() > prefix.size() ||
                    prefix.substr(0, preamble_.size()) != preamble_)
                    throw Error(502, "invalid_upstream", "Invalid stream preamble");
                if (preamble_.size() == prefix.size()) {
                    prefix_done_ = true;
                    preamble_.clear();
                }
                continue;
            }
        }
        if (!started_) {
            if (std::isspace(static_cast<unsigned char>(ch)))
                continue;
            if (ch != '[')
                throw Error(502, "invalid_upstream", "Expected outer stream array");
            started_ = true;
            depth_ = 1;
            preamble_ = "v";
            continue;
        }
        if (depth_ == 0) {
            if (!std::isspace(static_cast<unsigned char>(ch)))
                throw Error(502, "invalid_upstream", "Unexpected trailing stream data");
            continue;
        }
        if (!collecting_depth_) {
            if (std::isspace(static_cast<unsigned char>(ch)))
                continue;
            if (ch == ',') {
                if (preamble_.empty() || preamble_.back() != 'c')
                    throw Error(502, "invalid_upstream", "Misplaced comma in stream");
                preamble_.back() = 'r';
                continue;
            }
            if (ch == ']') {
                if (preamble_.empty() || preamble_.back() == 'r')
                    throw Error(502, "invalid_upstream", "Missing stream array value");
                preamble_.pop_back();
                --depth_;
                continue;
            }
            if (ch != '[' || preamble_.empty() || preamble_.back() == 'c')
                throw Error(502, "invalid_upstream", "Expected wire array value");
            preamble_.back() = 'c';
            ++depth_;
            if (depth_ == 3) {
                collecting_depth_ = 3;
                buffer_.assign(1, '[');
            } else
                preamble_.push_back('v');
            continue;
        }
        if (collecting_depth_) {
            if (buffer_.size() >= limit_)
                throw Error(502, "upstream_too_large",
                            "Wire chunk exceeds configured object limit");
            buffer_.push_back(ch);
        }
        if (quoted_) {
            if (escaped_)
                escaped_ = false;
            else if (ch == '\\')
                escaped_ = true;
            else if (ch == '"')
                quoted_ = false;
            continue;
        }
        if (ch == '"') {
            if (!collecting_depth_)
                throw Error(502, "invalid_upstream", "Unexpected string outside wire chunk");
            quoted_ = true;
            continue;
        }
        if (ch == '[' || ch == '{') {
            ++depth_;
            if (depth_ > 256)
                throw Error(502, "invalid_upstream", "Wire nesting limit exceeded");
            if (depth_ == 3 && !collecting_depth_) {
                if (ch != '[')
                    throw Error(502, "invalid_upstream", "Wire chunk must be an array");
                collecting_depth_ = depth_;
                buffer_.assign(1, ch);
            }
        } else if (ch == ']' || ch == '}') {
            if (depth_ == 0)
                throw Error(502, "invalid_upstream", "Unbalanced stream delimiter");
            bool complete = collecting_depth_ && depth_ == collecting_depth_;
            --depth_;
            if (complete) {
                boost::system::error_code ec;
                auto value = json::parse(buffer_, ec);
                if (ec || !value.is_array())
                    throw Error(502, "invalid_upstream", "Malformed wire JSON chunk");
                buffer_.clear();
                collecting_depth_ = 0;
                emit(std::move(value));
            }
        } else if (!collecting_depth_ && ch != ',' && !std::isspace(static_cast<unsigned char>(ch)))
            throw Error(502, "invalid_upstream", "Unexpected token outside wire chunk");
    }
}
void WireStreamParser::finish() {
    if (!prefix_done_ || !started_ || depth_ != 0 || quoted_ || escaped_ || collecting_depth_ ||
        !buffer_.empty())
        throw Error(502, "truncated_upstream", "Upstream stream ended inside JSON data");
}
std::size_t WireStreamParser::buffered_bytes() const { return buffer_.size() + preamble_.size(); }
} // namespace ais2api
