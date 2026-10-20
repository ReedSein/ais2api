// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/common.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <algorithm>
namespace ais2api::browser_detail {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
class Cdp {
public:
    asio::io_context io;
    websocket::stream<beast::tcp_stream> socket{io};
    void connect(unsigned short port, std::string target, std::stop_token token) {
        perform([&](auto callback) { beast::get_lowest_layer(socket).async_connect(tcp::endpoint(asio::ip::address_v4::loopback(), port), callback); }, token, 5s);
        socket.read_message_max(2 * 1024 * 1024);
        perform([&](auto callback) { socket.async_handshake("127.0.0.1:" + std::to_string(port), target, callback); }, token, 5s);
    }
    json::object call(std::string method, json::object params, std::string session, std::stop_token token, std::chrono::seconds timeout = 30s) {
        const auto id = ++next_;
        json::object request{{"id", id}, {"method", std::move(method)}, {"params", std::move(params)}};
        if (!session.empty()) request["sessionId"] = session;
        const auto message = json::serialize(request);
        perform([&](auto callback) { socket.async_write(asio::buffer(message), callback); }, token, timeout);
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            beast::flat_buffer buffer;
            const auto remaining = std::max(1s, std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now()));
            perform([&](auto callback) { socket.async_read(buffer, callback); }, token, remaining);
            beast::error_code parse_error;
            auto value = json::parse(beast::buffers_to_string(buffer.data()), parse_error);
            if (parse_error || !value.is_object()) { close(); throw Error(502, "browser_incompatible", "Invalid CDP reply"); }
            auto& object = value.as_object();
            auto response_id = object.if_contains("id");
            if (!response_id || !response_id->is_int64() || response_id->as_int64() != static_cast<std::int64_t>(id)) continue;
            if (!session.empty() && string_or(object, "sessionId") != session) { close(); throw Error(502, "browser_incompatible", "CDP reply belongs to a different page session"); }
            if (object.if_contains("error")) throw Error(502, "browser_incompatible", "Browser rejected CDP method " + string_or(request, "method"));
            auto result = object.if_contains("result");
            return result && result->is_object() ? result->as_object() : json::object{};
        }
        throw Error(504, "browser_timeout", "CDP command timed out");
    }
    json::value evaluate(std::string expression, const std::string& session, std::stop_token token, std::chrono::seconds timeout = 30s) {
        auto result = call("Runtime.evaluate", {{"expression", std::move(expression)}, {"awaitPromise", true}, {"returnByValue", true}}, session, token, timeout);
        if (result.if_contains("exceptionDetails")) throw Error(502, "browser_incompatible", "Page bridge failed or current AI Studio page is unsupported");
        auto remote = result.if_contains("result");
        if (!remote || !remote->is_object()) throw Error(502, "browser_incompatible", "Missing Runtime.evaluate result");
        auto value = remote->as_object().if_contains("value");
        return value ? *value : json::value{};
    }
    void close() noexcept { beast::error_code ec; beast::get_lowest_layer(socket).socket().close(ec); }
private:
    std::uint64_t next_ = 0;
    template<class Begin> void perform(Begin begin, std::stop_token token, std::chrono::seconds timeout) {
        check_cancel(token);
        bool done = false;
        beast::error_code error;
        io.restart();
        begin([&](beast::error_code ec, auto...) { error = ec; done = true; });
        const auto deadline = Clock::now() + timeout;
        while (!done && !token.stop_requested() && Clock::now() < deadline) { io.run_for(25ms); io.restart(); }
        if (!done) {
            close();
            while (!done) { io.run_for(25ms); io.restart(); }
            check_cancel(token);
            throw Error(504, "browser_timeout", "Browser communication timed out");
        }
        if (error) { close(); throw Error(502, "browser_disconnected", "Browser CDP connection failed"); }
    }
};


} // namespace ais2api::browser_detail
