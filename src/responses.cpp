// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/responses.hpp"
#include <algorithm>
namespace ais2api {
namespace {
[[noreturn]] void bad(std::string message) {
    throw Error(400, "invalid_request_error", std::move(message));
}
const json::object &obj(const json::value &v) {
    if (!v.is_object())
        bad("Expected an object");
    return v.as_object();
}
std::string required(const json::object &o, std::string_view k) {
    auto p = o.if_contains(k);
    if (!p || !p->is_string())
        bad(std::string(k) + " must be a string");
    return std::string(p->as_string());
}
void keys(const json::object &o, std::initializer_list<std::string_view> allowed) {
    for (auto &kv : o)
        if (std::find(allowed.begin(), allowed.end(), std::string_view(kv.key())) == allowed.end())
            bad("Unsupported Responses field: " + std::string(kv.key()));
}
json::value content(const json::value &v) {
    if (v.is_string())
        return v;
    if (!v.is_array())
        bad("Message content must be text or an array");
    json::array out;
    for (auto &x : v.as_array()) {
        auto &p = obj(x);
        auto t = required(p, "type");
        if (t == "input_text" || t == "output_text") {
            keys(p, {"type", "text", "annotations", "logprobs"});
            out.push_back(json::object{{"type", "text"}, {"text", required(p, "text")}});
        } else if (t == "input_image") {
            keys(p, {"type", "image_url", "detail"});
            auto url = required(p, "image_url");
            if (!url.starts_with("https://") && !url.starts_with("data:"))
                bad("input_image requires an HTTPS or data URL");
            json::object image{{"url", url}};
            if (auto d = p.if_contains("detail"))
                image["detail"] = *d;
            out.push_back(json::object{{"type", "image_url"}, {"image_url", image}});
        } else
            bad("Unsupported Responses content type: " + t);
    }
    return out;
}
std::string item_text(const json::object &i) {
    auto t = string_or(i, "type");
    if (t == "function_call")
        return string_or(i, "arguments");
    auto p = i.if_contains(t == "reasoning" ? "summary" : "content");
    if (!p || !p->is_array() || p->as_array().empty())
        return {};
    return string_or(p->as_array()[0].as_object(), "text");
}
} // namespace
json::object responses_to_chat(const json::object &in) {
    keys(in, {"model",
              "input",
              "instructions",
              "stream",
              "store",
              "background",
              "previous_response_id",
              "conversation",
              "tools",
              "tool_choice",
              "parallel_tool_calls",
              "max_output_tokens",
              "reasoning",
              "text",
              "temperature",
              "top_p",
              "metadata",
              "user",
              "ais2api",
              "include",
              "truncation",
              "service_tier"});
    for (auto k : {"store", "background"})
        if (auto p = in.if_contains(k); p && (!p->is_bool() || p->as_bool()))
            bad(std::string(k) + "=true is unsupported");
    for (auto k : {"previous_response_id", "conversation"})
        if (auto p = in.if_contains(k); p && !p->is_null())
            bad(std::string(k) + " is unsupported; provide full input history");
    if (auto p = in.if_contains("include"); p && (!p->is_array() || !p->as_array().empty()))
        bad("include is unsupported");
    if (auto p = in.if_contains("truncation"); p && *p != "disabled")
        bad("Only disabled truncation is supported");
    if (auto p = in.if_contains("service_tier"); p && *p != "auto")
        bad("Only auto service_tier is supported");
    if (auto p = in.if_contains("metadata")) {
        if (!p->is_object())
            bad("metadata must be an object of strings");
        if (p->as_object().size() > 16)
            bad("metadata supports at most 16 entries");
        for (auto &kv : p->as_object())
            if (kv.key().size() > 64 || !kv.value().is_string() ||
                kv.value().as_string().size() > 512)
                bad("metadata keys and values must be strings within Responses limits");
    }
    if (auto p = in.if_contains("parallel_tool_calls"); p && (!p->is_bool() || !p->as_bool()))
        bad("Only parallel_tool_calls=true is supported");
    json::object out{{"model", required(in, "model")}, {"store", false}};
    json::array messages;
    if (auto p = in.if_contains("instructions"); p && !p->is_null())
        messages.push_back(
            json::object{{"role", "system"}, {"content", required(in, "instructions")}});
    auto input = in.if_contains("input");
    if (!input)
        bad("input is required");
    if (input->is_string())
        messages.push_back(json::object{{"role", "user"}, {"content", *input}});
    else {
        if (!input->is_array())
            bad("input must be a string or array");
        for (auto &v : input->as_array()) {
            auto &i = obj(v);
            auto type = string_or(i, "type", "message");
            if (type == "message") {
                keys(i, {"type", "id", "status", "role", "content", "ais2api", "thought_signature",
                         "reasoning_content"});
                auto role = required(i, "role");
                if (role != "user" && role != "assistant" && role != "system" &&
                    role != "developer")
                    bad("Unsupported Responses message role");
                json::object m{{"role", role}};
                auto c = i.if_contains("content");
                if (!c)
                    bad("message content is required");
                m["content"] = content(*c);
                if (i.if_contains("ais2api")) {
                    std::string joined;
                    if (m.at("content").is_array())
                        for (auto &part : m.at("content").as_array())
                            joined += required(obj(part), "text");
                    else
                        joined = required(i, "content");
                    m["content"] = joined;
                }
                for (auto k : {"ais2api", "thought_signature", "reasoning_content"})
                    if (auto p = i.if_contains(k))
                        m[k] = *p;
                messages.push_back(m);
            } else if (type == "function_call") {
                keys(i, {"type", "id", "status", "call_id", "name", "arguments"});
                json::object function{{"name", required(i, "name")},
                                      {"arguments", required(i, "arguments")}};
                json::object call{
                    {"id", required(i, "call_id")}, {"type", "function"}, {"function", function}};
                messages.push_back(
                    json::object{{"role", "assistant"}, {"tool_calls", json::array{call}}});
            } else if (type == "function_call_output") {
                keys(i, {"type", "id", "status", "call_id", "output"});
                messages.push_back(json::object{{"role", "tool"},
                                                {"tool_call_id", required(i, "call_id")},
                                                {"content", required(i, "output")}});
            } else if (type == "reasoning") {
                keys(i, {"type", "id", "status", "summary", "thought_signature", "ais2api"});
                std::string text;
                if (auto p = i.if_contains("summary")) {
                    if (!p->is_array())
                        bad("reasoning summary must be an array");
                    for (auto &s : p->as_array()) {
                        auto &part = obj(s);
                        if (required(part, "type") != "summary_text")
                            bad("Unsupported reasoning summary");
                        text += required(part, "text");
                    }
                }
                json::object m{{"role", "assistant"}, {"reasoning_content", text}};
                if (auto ext = i.if_contains("ais2api"))
                    m["ais2api"] = *ext;
                if (auto p = i.if_contains("thought_signature"))
                    m["thought_signature"] = *p;
                if (!text.empty() || m.if_contains("thought_signature"))
                    messages.push_back(m);
            } else
                bad("Unsupported Responses input item: " + type);
        }
    }
    out["messages"] = messages;
    for (auto k :
         {"stream", "temperature", "top_p", "parallel_tool_calls", "metadata", "user", "ais2api"})
        if (auto p = in.if_contains(k))
            out[k] = *p;
    if (auto p = in.if_contains("max_output_tokens"))
        out["max_completion_tokens"] = *p;
    if (auto p = in.if_contains("reasoning"); p && !p->is_null()) {
        auto &r = obj(*p);
        keys(r, {"effort", "summary"});
        if (auto s = r.if_contains("summary"); s && !s->is_null())
            bad("Requested reasoning summaries are unsupported");
        if (auto e = r.if_contains("effort"))
            out["reasoning_effort"] = *e;
    }
    if (auto p = in.if_contains("text")) {
        auto &t = obj(*p);
        keys(t, {"format"});
        if (auto f = t.if_contains("format")) {
            auto format = obj(*f);
            auto type = required(format, "type");
            if (type == "json_schema") {
                format.erase("type");
                out["response_format"] = json::object{{"type", type}, {"json_schema", format}};
            } else {
                keys(format, {"type"});
                out["response_format"] = format;
            }
        }
    }
    if (auto p = in.if_contains("tools")) {
        if (!p->is_array())
            bad("tools must be an array");
        json::array tools;
        for (auto &v : p->as_array()) {
            auto f = obj(v);
            if (required(f, "type") != "function")
                bad("Only function tools are supported");
            keys(f, {"type", "name", "description", "parameters", "strict"});
            f.erase("type");
            tools.push_back(json::object{{"type", "function"}, {"function", f}});
        }
        out["tools"] = tools;
    }
    if (auto p = in.if_contains("tool_choice")) {
        if (p->is_object()) {
            auto &t = p->as_object();
            keys(t, {"type", "name"});
            if (required(t, "type") != "function")
                bad("Only function tool_choice is supported");
            out["tool_choice"] = json::object{
                {"type", "function"}, {"function", json::object{{"name", required(t, "name")}}}};
        } else
            out["tool_choice"] = *p;
    }
    return out;
}
json::object responses_options(const json::object &in) {
    json::object out;
    for (auto k : {"instructions", "user", "temperature", "top_p", "parallel_tool_calls",
                   "max_output_tokens", "reasoning", "text", "metadata", "tools", "tool_choice"})
        if (auto p = in.if_contains(k))
            out[k] = *p;
    return out;
}
json::object responses_response(const json::object &gemini, std::string_view model,
                                std::string_view id, const json::object &options, Store *store,
                                std::string_view principal) {
    auto chat = openai_response(gemini, model, id, store, principal);
    json::array output;
    std::string status = "completed";
    json::value incomplete = nullptr;
    if (!chat.at("choices").as_array().empty()) {
        auto &choice = chat.at("choices").as_array()[0].as_object();
        auto &m = choice.at("message").as_object();
        json::array native_text, native_reasoning;
        if (auto ext = m.if_contains("ais2api"))
            for (auto &part :
                 ext->as_object().at("gemini_content").as_object().at("parts").as_array())
                if (!part.as_object().if_contains("functionCall") &&
                    !bool_or(part.as_object(), "thought"))
                    native_text.push_back(part);
                else if (bool_or(part.as_object(), "thought"))
                    native_reasoning.push_back(part);
        auto reason = string_or(choice, "finish_reason");
        if (reason == "length" || reason == "content_filter") {
            status = "incomplete";
            incomplete = json::object{
                {"reason", reason == "length" ? "max_output_tokens" : "content_filter"}};
        }
        if (auto p = m.if_contains("reasoning_content");
            p && p->is_string() && !p->as_string().empty()) {
            json::object item{
                {"id", std::string(id) + "_reasoning"},
                {"type", "reasoning"},
                {"status", status},
                {"summary", json::array{json::object{{"type", "summary_text"}, {"text", *p}}}}};
            if (!native_reasoning.empty())
                item["ais2api"] =
                    json::object{{"gemini_content",
                                  json::object{{"role", "model"}, {"parts", native_reasoning}}}};
            output.push_back(item);
        }
        if (auto p = m.if_contains("content"); (p && p->is_string() && !p->as_string().empty()) ||
                                               m.if_contains("thought_signature") ||
                                               !native_text.empty()) {
            json::object item{{"id", std::string(id) + "_message"},
                              {"type", "message"},
                              {"role", "assistant"},
                              {"status", status},
                              {"content", json::array{json::object{
                                              {"type", "output_text"},
                                              {"text", p && p->is_string() ? *p : json::value("")},
                                              {"annotations", json::array{}}}}}};
            if (auto signature = m.if_contains("thought_signature")) {
                if (native_text.empty())
                    item["thought_signature"] = *signature;
                else
                    for (auto &part : native_text)
                        if (auto sig = part.as_object().if_contains("thoughtSignature"))
                            item["thought_signature"] = *sig;
            }
            if (!native_text.empty())
                item["ais2api"] = json::object{
                    {"gemini_content", json::object{{"role", "model"}, {"parts", native_text}}}};
            output.push_back(item);
        }
        if (auto p = m.if_contains("tool_calls")) {
            json::object seen_calls;
            std::size_t call_index = 0;
            for (auto &v : p->as_array()) {
                auto &c = v.as_object();
                auto &f = c.at("function").as_object();
                auto call_id = string_or(c, "id");
                if (auto seen = seen_calls.if_contains(call_id)) {
                    if (*seen != f)
                        throw Error(502, "upstream_protocol_error",
                                    "Repeated function call ID has conflicting arguments");
                    continue;
                }
                seen_calls[call_id] = f;
                output.push_back(
                    json::object{{"id", std::string(id) + "_fc_" + std::to_string(call_index++)},
                                 {"type", "function_call"},
                                 {"status", status},
                                 {"call_id", c.at("id")},
                                 {"name", f.at("name")},
                                 {"arguments", f.at("arguments")}});
            }
        }
    }
    json::object result{{"tools", json::array{}},
                        {"tool_choice", "auto"},
                        {"parallel_tool_calls", true},
                        {"instructions", nullptr},
                        {"max_output_tokens", nullptr},
                        {"text", json::object{{"format", json::object{{"type", "text"}}}}},
                        {"reasoning", json::object{{"effort", nullptr}, {"summary", nullptr}}},
                        {"temperature", 1.0},
                        {"top_p", 1.0},
                        {"metadata", json::object{}},
                        {"truncation", "disabled"},
                        {"user", nullptr}};
    for (auto &kv : options) {
        if ((kv.key() == "reasoning" || kv.key() == "text") && kv.value().is_object()) {
            for (auto &nested : kv.value().as_object())
                result.at(kv.key()).as_object()[nested.key()] = nested.value();
        } else
            result[kv.key()] = kv.value();
    }
    result["id"] = id;
    result["object"] = "response";
    result["created_at"] = unix_seconds();
    result["model"] = model;
    result["status"] = status;
    result["output"] = output;
    result["error"] = nullptr;
    result["incomplete_details"] = incomplete;
    result["store"] = false;
    result["background"] = false;
    result["previous_response_id"] = nullptr;
    result["usage"] = nullptr;
    if (auto u = chat.if_contains("usage")) {
        auto &x = u->as_object();
        json::object usage{{"input_tokens", x.at("prompt_tokens")},
                           {"output_tokens", x.at("completion_tokens")},
                           {"total_tokens", x.at("total_tokens")},
                           {"input_tokens_details", json::object{{"cached_tokens", 0}}},
                           {"output_tokens_details", json::object{{"reasoning_tokens", 0}}}};
        if (auto d = x.if_contains("prompt_tokens_details"))
            usage["input_tokens_details"] = *d;
        if (auto d = x.if_contains("completion_tokens_details"))
            usage["output_tokens_details"] = *d;
        result["usage"] = usage;
    }
    return result;
}
ResponsesStream::ResponsesStream(std::string model, std::string id, json::object options,
                                 std::size_t limit, Store *store, std::string principal)
    : model_(std::move(model)), id_(std::move(id)), principal_(std::move(principal)),
      options_(std::move(options)), limit_(limit), created_(unix_seconds()), store_(store) {
    if (!limit_)
        bad("Responses stream limit must be positive");
    if (json::serialize(options_).size() > limit_)
        throw Error(413, "response_too_large", "Responses options exceed configured object limit");
}
json::object ResponsesStream::event(std::string_view type, json::object fields) {
    fields["type"] = type;
    fields["sequence_number"] = sequence_++;
    return fields;
}
json::object ResponsesStream::response() {
    auto r = responses_response(aggregate_, model_, id_, options_, nullptr, principal_);
    r["created_at"] = created_;
    return r;
}
json::array ResponsesStream::start() {
    if (started_ || ended_)
        return {};
    started_ = true;
    auto r = response();
    r["status"] = "in_progress";
    return {event("response.created", {{"response", r}}),
            event("response.in_progress", {{"response", r}})};
}
json::array ResponsesStream::push(const json::object &delta) {
    if (ended_)
        return {};
    auto events = start();
    auto prior_aggregate = aggregate_;
    auto prior_output = output_;
    const auto prior_sequence = sequence_;
    try {
        merge_event(aggregate_, delta, limit_);
        auto r = response();
        auto next = r.at("output").as_array();
        // Preserve discovery order even if reasoning arrives after visible output.
        for (auto &v : next) {
            auto item = v.as_object();
            auto id = string_or(item, "id");
            std::size_t index = 0;
            for (; index < output_.size(); ++index)
                if (string_or(output_[index].as_object(), "id") == id)
                    break;
            auto type = string_or(item, "type");
            auto text = item_text(item);
            std::string previous;
            if (index == output_.size()) {
                auto initial = item;
                initial["status"] = "in_progress";
                if (type == "function_call")
                    initial["arguments"] = "";
                else {
                    auto key = type == "reasoning" ? "summary" : "content";
                    initial[key].as_array()[0].as_object()["text"] = "";
                }
                events.push_back(event("response.output_item.added",
                                       {{"output_index", index}, {"item", initial}}));
                output_.push_back(initial);
                if (type != "function_call") {
                    auto part =
                        initial.at(type == "reasoning" ? "summary" : "content").as_array()[0];
                    events.push_back(
                        event(type == "reasoning" ? "response.reasoning_summary_part.added"
                                                  : "response.content_part.added",
                              {{"item_id", id},
                               {"output_index", index},
                               {type == "reasoning" ? "summary_index" : "content_index", 0},
                               {"part", part}}));
                }
            } else
                previous = item_text(output_[index].as_object());
            if (!text.starts_with(previous))
                throw Error(502, "upstream_protocol_error",
                            "Responses output was revised by upstream");
            if (text.size() > previous.size()) {
                json::object fields{{"item_id", id},
                                    {"output_index", index},
                                    {"delta", text.substr(previous.size())}};
                if (type != "function_call")
                    fields[type == "reasoning" ? "summary_index" : "content_index"] = 0;
                events.push_back(
                    event(type == "function_call" ? "response.function_call_arguments.delta"
                          : type == "reasoning"   ? "response.reasoning_summary_text.delta"
                                                  : "response.output_text.delta",
                          fields));
            }
            item["status"] = "in_progress";
            output_[index] = item;
        }
        if (json::serialize(aggregate_).size() + json::serialize(output_).size() +
                json::serialize(options_).size() >
            limit_) {
            throw Error(413, "response_too_large",
                        "Responses stream exceeds configured object limit");
        }
        return events;
    } catch (...) {
        aggregate_ = std::move(prior_aggregate);
        output_ = std::move(prior_output);
        sequence_ = prior_sequence;
        throw;
    }
}
json::array ResponsesStream::finish() {
    if (ended_)
        return {};
    auto events = start();
    auto r = response();
    for (std::size_t i = 0; i < output_.size(); ++i) {
        auto &item = output_[i].as_object();
        item["status"] = r.at("status");
        auto t = string_or(item, "type");
        auto id = string_or(item, "id");
        json::object fields{{"item_id", id},
                            {"output_index", i},
                            {t == "function_call" ? "arguments" : "text", item_text(item)}};
        if (t != "function_call")
            fields[t == "reasoning" ? "summary_index" : "content_index"] = 0;
        events.push_back(event(t == "function_call" ? "response.function_call_arguments.done"
                               : t == "reasoning"   ? "response.reasoning_summary_text.done"
                                                    : "response.output_text.done",
                               fields));
        if (t != "function_call")
            events.push_back(
                event(t == "reasoning" ? "response.reasoning_summary_part.done"
                                       : "response.content_part.done",
                      {{"item_id", id},
                       {"output_index", i},
                       {t == "reasoning" ? "summary_index" : "content_index", 0},
                       {"part", item.at(t == "reasoning" ? "summary" : "content").as_array()[0]}}));
        events.push_back(event("response.output_item.done", {{"output_index", i}, {"item", item}}));
    }
    r["output"] = output_;
    events.push_back(
        event(string_or(r, "status") == "incomplete" ? "response.incomplete" : "response.completed",
              {{"response", r}}));
    ended_ = true;
    return events;
}
json::array ResponsesStream::fail(const Error &e) {
    if (ended_)
        return {};
    auto events = start();
    auto r = response();
    r["status"] = "failed";
    r["error"] = json::object{{"code", e.code}, {"message", e.what()}};
    for (auto &item : output_)
        item.as_object()["status"] = "incomplete";
    r["output"] = output_;
    events.push_back(event("response.failed", {{"response", r}}));
    ended_ = true;
    return events;
}
} // namespace ais2api
