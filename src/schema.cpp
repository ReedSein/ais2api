// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/schema.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace ais2api {
namespace {
[[noreturn]] void bad(std::string message) {
    throw Error(400, "invalid_argument", "schema: " + std::move(message));
}
// A null result denotes a valid reference whose capability is unsupported.
// This check never follows reference edges, so legal recursion remains valid.
const json::object *reference_target(const json::object &root, std::string_view ref) {
    if (ref.empty()) bad("empty $ref is not supported");
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const auto c = static_cast<unsigned char>(ref[i]);
        if (c <= 0x20 || c == 0x7f) bad("invalid URI in $ref");
        if (c == '%') {
            if (i + 2 >= ref.size() || !std::isxdigit(static_cast<unsigned char>(ref[i + 1])) || !std::isxdigit(static_cast<unsigned char>(ref[i + 2])))
                bad("invalid URI escape in $ref");
            i += 2;
        }
    }
    if (ref == "#") return &root;
    if (!ref.starts_with("#/")) return nullptr;
    const json::object *current = &root;
    ref.remove_prefix(2);
    while (true) {
        const auto slash = ref.find('/');
        auto token = ref.substr(0, slash);
        std::string key;
        for (std::size_t i = 0; i < token.size(); ++i) {
            if (token[i] != '~') key += token[i];
            else {
                if (++i == token.size() || (token[i] != '0' && token[i] != '1'))
                    bad("invalid JSON Pointer escape in $ref");
                key += token[i] == '0' ? '~' : '/';
            }
        }
        auto value = current->if_contains(key);
        if (value && value->is_bool() && slash == std::string_view::npos)
            return nullptr;
        if (!value || !value->is_object()) bad("$ref does not resolve to a schema object");
        current = &value->as_object();
        if (slash == std::string_view::npos) return current;
        ref.remove_prefix(slash + 1);
    }
}
void validate_reference(const json::object &root, const json::object &source) {
    auto ref = source.if_contains("$ref");
    if (!ref) return;
    if (!ref->is_string()) bad("$ref must be a string");
    const json::object *target = reference_target(root, std::string_view(ref->as_string()));
    std::set<const json::object *> seen;
    while (target && seen.insert(target).second) {
        if (seen.size() > 4096) bad("schema reference chain exceeds node limit");
        for (const auto &kv : source) {
            if (kv.key() == "$ref" || kv.key() == "description" || kv.key() == "title" || kv.key() == "default") continue;
            if (auto old = target->if_contains(kv.key()); old && *old != kv.value())
                bad("conflicting $ref siblings are not supported");
        }
        auto next = target->if_contains("$ref");
        if (!next) break;
        if (!next->is_string()) bad("$ref must be a string");
        target = reference_target(root, std::string_view(next->as_string()));
    }
}
// Validate the original tree before capability checks, so dropping a root or
// a keyword cannot hide malformed schemas or misspelled fields.
void validate(const json::value &value, const json::object &root, unsigned depth, std::size_t &nodes) {
    if (depth > 64 || ++nodes > 4096) bad("schema exceeds depth or node limit");
    if (value.is_bool()) return;
    if (!value.is_object()) bad("schema must be an object or boolean");
    validate_reference(root, value.as_object());
    for (const auto &kv : value.as_object()) {
        const auto key = std::string_view(kv.key());
        const auto &v = kv.value();
        auto require = [&](bool valid) { if (!valid) bad(std::string(key) + " has an invalid value type"); };
        if (key == "properties" || key == "$defs" || key == "definitions" || key == "patternProperties" || key == "dependentSchemas") {
            require(v.is_object());
            for (const auto &entry : v.as_object()) validate(entry.value(), root, depth + 1, nodes);
        } else if (key == "oneOf" || key == "anyOf" || key == "allOf" || key == "prefixItems") {
            require(v.is_array());
            for (const auto &entry : v.as_array()) validate(entry, root, depth + 1, nodes);
        } else if (key == "items" && v.is_array()) {
            for (const auto &entry : v.as_array()) validate(entry, root, depth + 1, nodes);
        } else if (key == "items" || key == "not" || key == "additionalProperties" || key == "additionalItems" || key == "contains" || key == "if" || key == "then" || key == "else" || key == "unevaluatedProperties" || key == "unevaluatedItems" || key == "contentSchema" || key == "propertyNames") {
            validate(v, root, depth + 1, nodes);
        } else if (key == "required" || key == "propertyOrdering") {
            require(v.is_array());
            for (const auto &entry : v.as_array()) require(entry.is_string());
        } else if (key == "dependencies" || key == "dependentRequired") {
            require(v.is_object());
            for (const auto &entry : v.as_object()) {
                if (entry.value().is_array()) {
                    for (const auto &name : entry.value().as_array()) require(name.is_string());
                } else {
                    require(key != "dependentRequired");
                    validate(entry.value(), root, depth + 1, nodes);
                }
            }
        } else if (key == "type") {
            require(v.is_string() || v.is_array());
            auto valid_type = [&](const json::value &t) {
                require(t.is_string());
                std::string name(t.as_string());
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                require(name == "string" || name == "number" || name == "integer" || name == "boolean" || name == "array" || name == "object" || name == "null");
            };
            if (v.is_array()) { require(!v.as_array().empty()); for (const auto &t : v.as_array()) valid_type(t); }
            else valid_type(v);
        } else if (key == "title" || key == "$schema" || key == "$id" || key == "$comment" || key == "$ref" || key == "$anchor" || key == "$dynamicAnchor" || key == "format" || key == "description" || key == "pattern" || key == "contentEncoding" || key == "contentMediaType") require(v.is_string());
        else if (key == "nullable" || key == "uniqueItems" || key == "readOnly" || key == "writeOnly" || key == "deprecated") require(v.is_bool());
        else if (key == "minimum" || key == "maximum" || key == "multipleOf") { require(v.is_number()); if (key == "multipleOf") require(json::value_to<double>(v) > 0); }
        else if (key == "exclusiveMinimum" || key == "exclusiveMaximum") require(v.is_number() || v.is_bool());
        else if (key == "minProperties" || key == "maxProperties" || key == "minLength" || key == "maxLength" || key == "minItems" || key == "maxItems" || key == "minContains" || key == "maxContains") require(v.is_uint64() || (v.is_int64() && v.as_int64() >= 0));
        else if (key == "enum" || key == "examples") require(v.is_array());
        else if (key != "default" && key != "const" && key != "example") bad("unsupported field " + std::string(key));
    }
}
class Lowerer {
    const json::object &root_;
    json::array *ignored_;
    std::string location_;
    bool ignore_unsupported_;
    std::set<const json::object *> active_;
    std::size_t nodes_ = 0;
    std::size_t bytes_ = 0;
    static constexpr std::size_t byte_limit = 8 * 1024 * 1024;

    void charge(std::size_t size) {
        if (size > byte_limit - bytes_) bad("schema expansion exceeds byte limit");
        bytes_ += size;
    }

    void ignored(std::string_view keyword) {
        if (!ignored_) return;
        json::value name(location_ + "." + std::string(keyword));
        if (std::find(ignored_->begin(), ignored_->end(), name) == ignored_->end())
            ignored_->push_back(std::move(name));
    }
    const json::object &resolve(std::string_view ref) {
        if (auto target = reference_target(root_, ref)) return *target;
        throw UnsupportedSchema("remote, anchor or boolean reference schema is not supported");
    }
    void hint(json::object &schema, std::string text) {
        charge(text.size());
        auto p = schema.if_contains("description");
        if (p && !p->is_string()) bad("description must be a string");
        auto description = p ? std::string(p->as_string()) : std::string{};
        if (!description.empty()) description += '\n';
        schema["description"] = description + text;
    }
public:
    Lowerer(const json::object &root, json::array *ignored, std::string_view location, bool ignore_unsupported)
        : root_(root), ignored_(ignored), location_(location), ignore_unsupported_(ignore_unsupported) {}

    json::object lower(const json::object &source, unsigned depth = 0) {
        if (depth > 64 || ++nodes_ > 4096) bad("schema expansion exceeds depth or node limit");
        // Charge before copying, including each repeated reference target.
        // This bounds live intermediate copies, not just the final result.
        charge(json::serialize(source).size());
        if (!active_.insert(&source).second) throw UnsupportedSchema("recursive $ref is not supported by the upstream");
        auto schema = source;
        if (auto ref = source.if_contains("$ref")) {
            if (!ref->is_string()) bad("$ref must be a string");
            auto target = lower(resolve(std::string_view(ref->as_string())), depth + 1);
            schema.erase("$ref");
            // Metadata siblings are common in Pydantic schemas. Conflicting
            // validation siblings require intersection semantics; do not weaken them.
            for (const auto &kv : schema) {
                auto old = target.if_contains(kv.key());
                if (old && *old != kv.value() && kv.key() != "description" &&
                    kv.key() != "title" && kv.key() != "default")
                    bad("conflicting $ref siblings are not supported");
                target[kv.key()] = kv.value();
            }
            schema = std::move(target);
        }
        // Validate known unsupported keyword values before dropping constraints.
        // Unknown fields remain errors even in compatibility mode.
        for (auto it = schema.begin(); it != schema.end();) {
            const std::string key(it->key());
            const auto &v = it->value();
            bool known = true, valid = true;
            if (key == "exclusiveMinimum" || key == "exclusiveMaximum") valid = v.is_number() || v.is_bool();
            else if (key == "multipleOf") valid = v.is_number() && json::value_to<double>(v) > 0;
            else if (key == "uniqueItems" || key == "readOnly" || key == "writeOnly" || key == "deprecated") valid = v.is_bool();
            else if (key == "minContains" || key == "maxContains") valid = (v.is_uint64() || (v.is_int64() && v.as_int64() >= 0));
            else if (key == "contentEncoding" || key == "contentMediaType" || key == "$anchor" || key == "$dynamicAnchor") valid = v.is_string();
            else if (key == "examples") valid = v.is_array();
            else if (key == "patternProperties" || key == "dependentSchemas" || key == "dependencies" || key == "dependentRequired") {
                valid = v.is_object();
                if (valid) for (const auto &entry : v.as_object()) {
                    const auto &x = entry.value();
                    if (key == "dependentRequired" || (key == "dependencies" && x.is_array())) {
                        if (!x.is_array()) valid = false;
                        else for (const auto &name : x.as_array()) valid = valid && name.is_string();
                    } else valid = valid && (x.is_object() || x.is_bool());
                }
            } else if (key == "prefixItems") {
                valid = v.is_array();
                if (valid) for (const auto &x : v.as_array()) valid = valid && (x.is_object() || x.is_bool());
            } else if (key == "additionalItems" || key == "contains" || key == "if" || key == "then" || key == "else" || key == "unevaluatedProperties" || key == "unevaluatedItems" || key == "contentSchema" || key == "propertyNames") valid = v.is_object() || v.is_bool();
            else known = false;
            if (!known) { ++it; continue; }
            if (!valid) bad(key + " has an invalid value type");
            if (!ignore_unsupported_) bad("unsupported field " + key);
            ignored(key);
            it = schema.erase(it);
        }
        for (auto keyword : {"$defs", "definitions"}) {
            if (auto p = schema.if_contains(keyword); p && !p->is_object())
                bad(std::string(keyword) + " must be an object");
            schema.erase(keyword);
        }
        for (auto keyword : {"title", "$schema", "$id", "$comment"}) {
            if (auto p = schema.if_contains(keyword)) {
                if (!p->is_string()) bad(std::string(keyword) + " must be a string");
                if (std::string_view(keyword) == "title" && !p->as_string().empty())
                    hint(schema, "Title: " + std::string(p->as_string()));
                ignored(keyword);
                schema.erase(keyword);
            }
        }
        if (auto p = schema.if_contains("default")) {
            hint(schema, "Default: " + json::serialize(*p));
            ignored("default");
            schema.erase("default");
        }
        if (auto p = schema.if_contains("additionalProperties")) {
            if (!p->is_bool() && !p->is_object())
                bad("additionalProperties must be boolean or a schema object");
            if (!schema.if_contains("type")) schema["type"] = "object";
            p = schema.if_contains("additionalProperties");
            if (p->is_object()) {
                auto additional = lower(p->as_object(), depth + 1);
                hint(schema, "Additional property values follow this schema: " + json::serialize(additional));
            } else if (!p->as_bool())
                hint(schema, "Do not include properties other than those declared in properties.");
            ignored("additionalProperties");
            schema.erase("additionalProperties");
        }
        if (auto p = schema.if_contains("const")) {
            auto value = *p;
            if (auto values = schema.if_contains("enum"); values &&
                (!values->is_array() || std::find(values->as_array().begin(), values->as_array().end(), value) == values->as_array().end()))
                bad("const conflicts with enum");
            schema["enum"] = json::array{value};
            schema.erase("const");
        }
        if (!schema.if_contains("type"))
            if (auto values = schema.if_contains("enum"); values && values->is_array() && !values->as_array().empty()) {
                const auto &first = values->as_array().front();
                std::string type = first.is_string() ? "string" : first.is_bool() ? "boolean" :
                    (first.is_int64() || first.is_uint64()) ? "integer" : first.is_number() ? "number" : "";
                bool homogeneous = !type.empty();
                for (const auto &v : values->as_array()) {
                    if (type == "string") homogeneous = homogeneous && v.is_string();
                    else if (type == "boolean") homogeneous = homogeneous && v.is_bool();
                    else if (type == "integer") homogeneous = homogeneous && (v.is_int64() || v.is_uint64());
                    else if (type == "number") homogeneous = homogeneous && v.is_number();
                }
                if (homogeneous) schema["type"] = type;
            }
        // The private Schema enum field accepts strings only. Keep other
        // scalar enum values as explicit model guidance in non-strict mode.
        if (auto values = schema.if_contains("enum"); values && values->is_array()) {
            bool strings = true;
            for (const auto &v : values->as_array()) strings = strings && v.is_string();
            if (!strings) {
                hint(schema, "Allowed values: " + json::serialize(*values));
                ignored("enum");
                schema.erase("enum");
            }
        }
        for (auto keyword : {"properties"})
            if (auto p = schema.if_contains(keyword)) {
                if (!p->is_object()) bad("properties must be an object");
                for (auto &kv : p->as_object()) {
                    if (kv.value().is_bool()) throw UnsupportedSchema("boolean property schema is not supported");
                    if (!kv.value().is_object()) bad("property schema must be an object");
                    kv.value() = lower(kv.value().as_object(), depth + 1);
                }
            }
        for (auto keyword : {"items", "not"})
            if (auto p = schema.if_contains(keyword)) {
                if (p->is_bool() || (std::string_view(keyword) == "items" && p->is_array()))
                    throw UnsupportedSchema("boolean or tuple schema is not supported");
                if (!p->is_object()) bad(std::string(keyword) + " must be a schema object");
                *p = lower(p->as_object(), depth + 1);
            }
        for (auto keyword : {"oneOf", "anyOf", "allOf"})
            if (auto p = schema.if_contains(keyword)) {
                if (!p->is_array()) bad(std::string(keyword) + " must be an array");
                for (auto &branch : p->as_array()) {
                    if (branch.is_bool()) throw UnsupportedSchema("boolean union schema is not supported");
                    if (!branch.is_object()) bad("union branch must be a schema object");
                    branch = lower(branch.as_object(), depth + 1);
                }
            }
        // Also validate schemas used only as textual additionalProperties
        // guidance; these do not pass through the native encoder themselves.
        for (const auto &kv : schema) {
            static constexpr std::string_view allowed[] = {
                "type", "format", "description", "nullable", "enum", "items", "properties", "required",
                "minProperties", "maxProperties", "minimum", "maximum", "minLength", "maxLength", "pattern",
                "example", "oneOf", "anyOf", "allOf", "not", "maxItems", "minItems", "propertyOrdering"};
            if (std::find(std::begin(allowed), std::end(allowed), std::string_view(kv.key())) == std::end(allowed))
                bad("unsupported field " + std::string(kv.key()));
        }
        if (auto type = schema.if_contains("type")) {
            if (type->is_array()) {
                unsigned concrete = 0;
                for (const auto &t : type->as_array()) {
                    if (!t.is_string()) bad("type array must contain strings");
                    std::string name(t.as_string());
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (name != "string" && name != "number" && name != "integer" && name != "boolean" && name != "array" && name != "object" && name != "null") bad("invalid schema type");
                    if (name != "null") ++concrete;
                }
                if (concrete != 1) throw UnsupportedSchema("multi-type schema is not supported");
            } else if (!type->is_string()) bad("type must be a string or array");
        } else if (!schema.if_contains("properties") && !schema.if_contains("items") && !schema.if_contains("oneOf") && !schema.if_contains("anyOf") && !schema.if_contains("allOf"))
            throw UnsupportedSchema("schema requires a concrete type");
        active_.erase(&source);
        return schema;
    }
};
}
json::object prepare_schema(const json::object &schema, json::array *ignored, std::string_view location, bool ignore_unsupported) {
    if (json::serialize(schema).size() > 8 * 1024 * 1024) bad("schema expansion exceeds byte limit");
    std::size_t nodes = 0;
    validate(json::value(schema), schema, 0, nodes);
    return Lowerer(schema, ignored, location, ignore_unsupported).lower(schema);
}
}
