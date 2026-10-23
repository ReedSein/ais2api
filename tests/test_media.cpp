// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/media.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/asio.hpp>
#include <array>
#include <mutex>
#include <thread>
using namespace ais2api;
namespace {
using Address = boost::asio::ip::address;
Address ip(const char* text) { return boost::asio::ip::make_address(text); }
struct Proxy {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor{io, {ip("127.0.0.1"), 0}};
    std::mutex mutex;
    std::vector<std::string> requests;
    std::jthread worker;
    Proxy() {
        acceptor.non_blocking(true);
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                boost::asio::ip::tcp::socket socket(io);
                boost::system::error_code ec;
                acceptor.accept(socket, ec);
                if (ec) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
                socket.non_blocking(true);
                std::string request;
                const auto deadline = Clock::now() + std::chrono::seconds(2);
                while (!stop.stop_requested() && Clock::now() < deadline && request.size() < 8192 && request.find("\r\n\r\n") == request.npos) {
                    std::array<char, 1024> buffer{};
                    auto count = socket.read_some(boost::asio::buffer(buffer), ec);
                    if (!ec) request.append(buffer.data(), count);
                    else if (ec != boost::asio::error::would_block && ec != boost::asio::error::try_again) break;
                    else std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                { std::lock_guard lock(mutex); requests.push_back(request); }
                const std::string reply = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                boost::asio::write(socket, boost::asio::buffer(reply), ec);
            }
        });
    }
    ~Proxy() { worker.request_stop(); worker.join(); }
    std::string url() { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()); }
    std::vector<std::string> captured() { std::lock_guard lock(mutex); return requests; }
};
}
TEST_CASE("media accepts global IPv6 while rejecting special destinations", "[media]") {
    for (const auto* text : {"2400:cb00::1", "2606:4700:4700::1111", "2a00:1450::1", "3000::1", "8.8.8.8", "::ffff:8.8.8.8"})
        CHECK(media_detail::public_address(ip(text)));
    for (const auto* text : {"::", "::1", "fc00::1", "fe80::1", "ff02::1", "2001::1", "2001:2::1", "2001:db8::1", "2002:7f00:1::1", "3fff::1", "3fff:fff::1", "4000::1", "::ffff:127.0.0.1", "::ffff:10.0.0.1", "127.0.0.1", "10.0.0.1", "169.254.169.254"})
        CHECK_FALSE(media_detail::public_address(ip(text)));
}
TEST_CASE("media proxies connect only to validated numeric destinations", "[media]") {
    Proxy proxy;
    int resolutions = 0;
    const media_detail::Resolver resolver = [&](std::string_view host) {
        CHECK(host == "media.invalid");
        ++resolutions;
        return std::vector<Address>{ip("2400:cb00::1"), ip("8.8.8.8")};
    };
    REQUIRE_THROWS_AS(media_detail::fetch_with_resolver("https://media.invalid/image", 1024, proxy.url(), {}, resolver), Error);
    CHECK(resolutions == 1);
    const auto requests = proxy.captured();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].starts_with("CONNECT [2400:cb00::1]:443 HTTP/1.1\r\n"));
    CHECK(requests[1].starts_with("CONNECT 8.8.8.8:443 HTTP/1.1\r\n"));
    for (const auto& request : requests) CHECK(request.find("media.invalid") == request.npos);
}
TEST_CASE("media rejects every private resolver answer before contacting a proxy", "[media]") {
    Proxy proxy;
    const media_detail::Resolver resolver = [](std::string_view) {
        return std::vector<Address>{ip("8.8.8.8"), ip("127.0.0.1")};
    };
    try {
        media_detail::fetch_with_resolver("https://media.invalid/image", 1024, proxy.url(), {}, resolver);
        FAIL("private resolver answer was accepted");
    } catch (const Error& error) {
        CHECK(error.status == 400);
        CHECK(error.code == "media_url");
    }
    CHECK(proxy.captured().empty());
}

TEST_CASE("media IPv6 literals bypass DNS and scoped URLs are rejected", "[media]") {
    Proxy proxy;
    const media_detail::Resolver no_dns = [](std::string_view) -> std::vector<Address> {
        FAIL("numeric image addresses must not require DNS");
        return {};
    };
    try {
        media_detail::fetch_with_resolver("https://[2400:cb00::1]/image", 1024, proxy.url(), {}, no_dns);
        FAIL("synthetic proxy must reject the tunnel");
    } catch (const Error& error) {
        CHECK(error.status == 502);
        CHECK(error.code == "media_fetch");
    }
    const auto requests = proxy.captured();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].starts_with("CONNECT [2400:cb00::1]:443 HTTP/1.1\r\n"));
    for (auto url : {"https://[2400:cb00::1%251]/image", "https://[fe80::1%251]/image"}) {
        try {
            media_detail::fetch_with_resolver(url, 1024, proxy.url(), {}, no_dns);
            FAIL("scoped image address must be rejected");
        } catch (const Error& error) {
            CHECK(error.status == 400);
            CHECK(error.code == "media_url");
        }
    }
    CHECK(proxy.captured().size() == 1);
}
