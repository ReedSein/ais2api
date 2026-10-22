// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/common.hpp"
#include <array>
#include <functional>
#include <openssl/evp.h>

namespace ais2api::browser_detail {
class GoogleAuthorizationCache {
public:
    std::string authorization(const std::function<json::array()>& refresh,
                              std::int64_t timestamp = unix_seconds(), Clock::time_point now = Clock::now()) {
        bool expired = false;
        for (auto& cookie : cookies_) {
            if (!cookie.value.empty() && cookie.expires >= 0 && cookie.expires <= static_cast<double>(timestamp)) {
                cookie = {}; expired = true;
            }
        }
        if (expired || !has_signature() || !refreshed_ || now - *refreshed_ >= std::chrono::seconds(60)) {
            try {
                const auto received = refresh();
                std::array<Cookie, 3> replacement{};
                for (const auto& item : received) {
                    if (!item.is_object()) continue;
                    const auto& object = item.as_object();
                    const auto name = string_or(object, "name");
                    for (std::size_t i = 0; i < names_.size(); ++i) if (name == names_[i]) {
                        auto value = string_or(object, "value");
                        double expires = -1;
                        if (const auto* field = object.if_contains("expires"); field && field->is_number())
                            expires = json::value_to<double>(*field);
                        if (!value.empty() && (expires < 0 || expires > static_cast<double>(timestamp)))
                            replacement[i] = {std::move(value), expires};
                    }
                }
                cookies_ = std::move(replacement);
                refreshed_ = now;
            } catch (const Error& error) {
                if (error.code == "cancelled") throw;
            }
        }
        if (!has_signature()) return {};
        const auto& main = cookies_[0].value.empty() ? cookies_[1].value : cookies_[0].value;
        const auto time = std::to_string(timestamp);
        std::string result;
        constexpr std::array<std::string_view, 3> schemes{"SAPISIDHASH", "SAPISID1PHASH", "SAPISID3PHASH"};
        for (std::size_t i = 0; i < schemes.size(); ++i) {
            const auto& value = i == 0 || cookies_[i].value.empty() ? main : cookies_[i].value;
            const auto input = time + " " + value + " https://aistudio.google.com";
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int size = 0;
            if (EVP_Digest(input.data(), input.size(), digest.data(), &size, EVP_sha1(), nullptr) != 1)
                throw Error(500, "authorization_hash_failed", "Could not generate browser authorization");
            if (!result.empty()) result += ' ';
            result += schemes[i]; result += ' '; result += time; result += '_';
            constexpr char hex[] = "0123456789abcdef";
            for (unsigned int n = 0; n < size; ++n) { result += hex[digest[n] >> 4]; result += hex[digest[n] & 15]; }
        }
        return result;
    }
    void clear() noexcept { cookies_ = {}; refreshed_.reset(); }
private:
    struct Cookie { std::string value; double expires = -1; };
    inline static constexpr std::array<std::string_view, 3> names_{"SAPISID", "__Secure-1PAPISID", "__Secure-3PAPISID"};
    std::array<Cookie, 3> cookies_{};
    std::optional<Clock::time_point> refreshed_;
    bool has_signature() const { return !cookies_[0].value.empty() || !cookies_[1].value.empty(); }
};
} // namespace ais2api::browser_detail
