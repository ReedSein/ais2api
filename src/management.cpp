// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/management.hpp"
#include <algorithm>
#include <cctype>

namespace ais2api {
namespace {
std::string identifier(const json::object& command) {
    auto id = string_or(command, "id");
    if (id.empty()) throw Error(400, "invalid_command", "An id is required");
    return id;
}

json::object account_summary(const json::object& input) {
    json::object output;
    for (auto key : {"id", "name", "email", "auth_user", "disabled", "expired", "failures"}) {
        if (auto value = input.if_contains(key)) output[key] = *value;
    }
    return output;
}

std::string redact_proxy(std::string proxy) {
    auto begin = proxy.find("://");
    begin = begin == std::string::npos ? 0 : begin + 3;
    auto end = proxy.find_first_of("/?#", begin);
    auto at = proxy.rfind('@', end == std::string::npos ? proxy.size() : end);
    if (at != std::string::npos && at >= begin)
        proxy.replace(begin, at - begin, "***");
    if (auto query = proxy.find_first_of("?#"); query != std::string::npos)
        proxy.erase(query);
    return proxy;
}

bool sensitive_key(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "token" || key.ends_with("_token") || key == "accesstoken" || key == "refreshtoken") return true;
    for (auto fragment : {"password", "secret", "cookie", "authorization", "api_key", "apikey", "thoughtsignature"})
        if (key.find(fragment) != std::string::npos) return true;
    return false;
}

void redact(json::value& value) {
    if (value.is_array()) {
        for (auto& item : value.as_array()) redact(item);
    } else if (value.is_object()) {
        for (auto& entry : value.as_object()) {
            if (sensitive_key(std::string(entry.key()))) entry.value() = "***";
            else if (entry.key() == "proxy" && entry.value().is_string())
                entry.value() = redact_proxy(std::string(entry.value().as_string()));
            else redact(entry.value());
        }
    }
}
}

json::object manage(Store& store, Config& config, const json::object& command) {
    auto name = string_or(command, "command");
    if (name == "accounts.list") {
        json::array accounts;
        for (auto& item : store.account_status()) accounts.push_back(account_summary(item.as_object()));
        return {{"accounts", accounts}};
    }
    if (name == "accounts.import") {
        auto input = command.if_contains("input");
        if (!input) throw Error(400, "invalid_command", "Account import input is required");
        auto imported = store.import_accounts(*input, string_or(command, "name"));
        json::array accounts;
        for (auto& item : imported) accounts.push_back(account_summary(item.as_object()));
        return {{"imported", accounts.size()}, {"accounts", accounts}};
    }
    if (name == "accounts.select" || name == "accounts.remove" || name == "accounts.reset") {
        auto id = identifier(command);
        // Resolve first so removal/reset of an unknown id cannot claim success.
        (void)store.account(id);
        if (name == "accounts.select") store.select_account(id);
        else if (name == "accounts.remove") store.remove_account(id);
        else store.reset_account(id);
        return {{"ok", true}, {"id", id}};
    }
    if (name == "keys.create") return store.create_key(string_or(command, "name", "API key"));
    if (name == "keys.list") return {{"keys", store.keys()}};
    if (name == "keys.revoke") {
        auto id = identifier(command);
        bool found = false;
        for (auto& key : store.keys())
            if (string_or(key.as_object(), "id") == id) found = true;
        if (!found) throw Error(404, "key_not_found", "API key not found");
        store.revoke_key(id);
        return {{"ok", true}, {"id", id}};
    }
    if (name == "stats") return store.stats();
    if (name == "config.show") {
        json::value value(config.to_json());
        redact(value);
        return {{"config", value}, {"config_file", config.config_file.string()}};
    }
    if (name == "config.validate") {
        config.validate();
        return {{"valid", true}, {"config_file", config.config_file.string()}};
    }
    throw Error(400, "invalid_command", "Unknown management command");
}
}
