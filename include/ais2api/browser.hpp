// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "backend.hpp"
#include <memory>
namespace ais2api {
class BrowserGateway final : public GenerationBackend {
  public:
    BrowserGateway(const Config &config, Store &store);
    ~BrowserGateway() override;
    void generate(const Request &request, const Account &account, const EventSink &sink,
                  std::stop_token cancel) override;
    json::array models(std::stop_token cancel = {}) override;
    json::object probe(const Account &account, std::stop_token cancel = {}) override;
    json::object diagnostics() override;
    void reconfigure(const Config &config) override;
    json::object verify_runtime(std::stop_token cancel = {});
    void close() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ais2api
