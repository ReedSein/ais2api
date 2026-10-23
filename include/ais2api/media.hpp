// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "common.hpp"
#include <boost/asio/ip/address.hpp>

namespace ais2api::media_detail {
using Resolver = std::function<std::vector<boost::asio::ip::address>(std::string_view)>;
bool public_address(const boost::asio::ip::address& address);
// An optional resolver keeps destination validation testable without external DNS.
std::string fetch_with_resolver(std::string_view url, std::size_t limit,
    std::string_view proxy, std::stop_token cancel, const Resolver& resolver = {});
}
