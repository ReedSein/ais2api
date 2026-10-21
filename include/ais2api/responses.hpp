// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "protocol.hpp"
namespace ais2api {
json::object responses_to_chat(const json::object &input);
json::object responses_options(const json::object &input);
json::object responses_response(const json::object &gemini, std::string_view model,
                                std::string_view id, const json::object &options,
                                Store *store = nullptr, std::string_view principal = {});
class ResponsesStream {
  public:
    ResponsesStream(std::string model, std::string id, json::object options, std::size_t byte_limit,
                    Store *store = nullptr, std::string principal = {});
    json::array start();
    json::array push(const json::object &gemini_delta);
    json::array finish();
    json::array fail(const Error &error);

  private:
    json::object event(std::string_view type, json::object fields);
    json::object response();
    std::string model_, id_, principal_;
    json::object options_, aggregate_;
    json::array output_;
    std::size_t limit_, sequence_ = 0;
    std::int64_t created_;
    Store *store_;
    bool started_ = false, ended_ = false;
};
} // namespace ais2api
