// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "protocol.hpp"

namespace ais2api {
// A generation session boundary. Production uses BrowserGateway; deterministic
// transports can exercise scheduling and HTTP cancellation without Google.
class GenerationBackend {
  public:
    virtual ~GenerationBackend() = default;
    virtual void generate(const Request &, const Account &, const EventSink &, std::stop_token) = 0;
    virtual json::array models(std::stop_token = {}) = 0;
    virtual json::object probe(const Account &, std::stop_token = {}) = 0;
    virtual json::object probe_import(Account &account, std::stop_token token = {}) { return probe(account, token); }
    virtual json::object diagnostics() = 0;
    virtual void reconfigure(const Config &) {}
    virtual void close() = 0;
    // Discard the resident account's page and BotGuard session so the next
    // request rebuilds it in place, keeping the browser process when possible.
    virtual void reset_session() { close(); }
};
} // namespace ais2api
