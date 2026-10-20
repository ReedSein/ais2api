// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "store.hpp"
namespace ais2api {
enum class Protocol { gemini, openai };
struct Request {
    Protocol protocol = Protocol::gemini;
    std::string model, principal;
    bool stream = false;
    bool include_usage = false;
    json::object gemini;
};
using EventSink = std::function<void(json::object)>;
Request normalize_gemini(std::string model, const json::object& body, bool stream, const Config& config);
Request normalize_openai(const json::object& body, const Config& config, Store* store = nullptr, std::string principal = {});
json::array encode_contents(const json::array& contents);
json::array encode_schema(const json::object& schema);
json::array encode_request(const Request& request, const json::array& base_template, std::string_view browser_snapshot);
json::object decode_chunk(const json::value& wire_chunk);
bool has_content(const json::object& event);
void merge_event(json::object& aggregate, const json::object& event, std::size_t byte_limit);
json::object openai_response(const json::object& gemini, std::string_view model, std::string_view id, Store* store = nullptr, std::string_view principal = {});
json::object openai_delta(const json::object& gemini_delta, std::string_view model, std::string_view id, Store* store = nullptr, std::string_view principal = {});
class WireStreamParser {
public:
    explicit WireStreamParser(std::size_t max_object_bytes = 64 * 1024 * 1024);
    void feed(std::string_view bytes, const std::function<void(json::value)>& emit);
    void finish();
    std::size_t buffered_bytes() const;
private:
    std::size_t limit_, depth_ = 0;
    std::string buffer_, preamble_;
    bool quoted_ = false, escaped_ = false, started_ = false, prefix_done_ = false;
    std::size_t collecting_depth_ = 0;
};
}
