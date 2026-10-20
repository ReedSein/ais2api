// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "config.hpp"
#include <memory>
namespace ais2api {
struct Account {
    std::string id, name, email;
    std::string auth_user = "0";
    json::object storage_state;
    bool disabled = false;
};
class Store {
public:
    explicit Store(const fs::path& directory);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    std::vector<Account> accounts();
    Account account(std::string_view id);
    json::array import_accounts(const json::value& input, std::string name = {});
    void put_account(const Account& account);
    void remove_account(std::string_view id);
    void select_account(std::string_view id);
    void reset_account(std::string_view id);
    std::optional<Account> choose(std::string_view model, const std::vector<std::string>& excluded = {});
    void failure(std::string_view account, std::string_view model, std::string_view kind, unsigned retry_seconds = 0);
    void success(std::string_view account, std::string_view model, const json::object& usage);
    json::object stats();
    json::array account_status();
    json::object create_key(std::string name);
    json::array keys();
    void revoke_key(std::string_view id);
    std::optional<std::string> authenticate(std::string_view secret);
    void put_tool(std::string_view principal, std::string_view call_id, const json::object& metadata);
    std::optional<json::object> get_tool(std::string_view principal, std::string_view call_id);
    void backup(const fs::path& target);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
