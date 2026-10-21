// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include "ais2api/store.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <thread>
using namespace ais2api;
namespace net=boost::asio;
namespace http=boost::beast::http;
using tcp=net::ip::tcp;
using namespace std::chrono_literals;
namespace {
struct LiveServer {
    fs::path directory=fs::temp_directory_path()/("ais2api-http-"+random_hex(8));
    Config config;
    std::string key;
    std::jthread thread;
    std::exception_ptr failure;
    explicit LiveServer(bool configured_key=true){
        net::io_context io;tcp::acceptor port(io,{tcp::v4(),0});auto free_port=port.local_endpoint().port();port.close();
        config.data_dir=directory/"data";config.config_file=directory/"config.json";config.port=free_port;config.configured_models={"test-explicit-model"};config.limits.request_bytes=4096;config.limits.object_bytes=8192;config.limits.relay_bytes=1024;
        key="http-test-"+random_hex(24);config.api_key=configured_key?key:"";
        fs::create_directories(directory);atomic_write(config.config_file,json::serialize(config.to_json()),true);
        thread=std::jthread([this]{try{serve(config);}catch(...){failure=std::current_exception();}});
        bool ready=false;for(unsigned i=0;i<100;++i){try{local_call(config,{{"command","status"}});ready=true;break;}catch(const Error&){}std::this_thread::sleep_for(20ms);}
        if(!ready){if(thread.joinable())thread.join();if(failure)std::rethrow_exception(failure);throw std::runtime_error("HTTP test service did not start");}
    }
    ~LiveServer(){try{local_call(config,{{"command","stop"}});}catch(...){}if(thread.joinable())thread.join();std::error_code ec;fs::remove_all(directory,ec);}
    http::response<http::string_body> call(http::verb method,std::string target,std::string body={},std::string auth={}){
        net::io_context io;tcp::socket socket(io);socket.connect({net::ip::make_address("127.0.0.1"),config.port});http::request<http::string_body> request(method,target,11);request.set(http::field::host,"127.0.0.1");request.set(http::field::content_type,"application/json");if(!auth.empty())request.set(http::field::authorization,"Bearer "+auth);request.body()=std::move(body);request.prepare_payload();http::write(socket,request);boost::beast::flat_buffer input;http::response<http::string_body> response;http::read(socket,input,response);return response;
    }
};
}
TEST_CASE("HTTP exposes data routes and enforces authentication without Google","[http]"){
    LiveServer server;
    REQUIRE(server.call(http::verb::get,"/health").result_int()==200);
    REQUIRE(server.call(http::verb::get,"/ready").result_int()==503);
    REQUIRE(server.call(http::verb::get,"/v1/models").result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1beta/models",{},"invalid").result_int()==401);
    auto models=server.call(http::verb::get,"/v1/models",{},server.key);REQUIRE(models.result_int()==200);auto data=json::parse(models.body()).as_object().at("data").as_array();REQUIRE(data.size()==1);REQUIRE(string_or(data[0].as_object(),"id")=="test-explicit-model");
    REQUIRE(server.call(http::verb::get,"/v1/models/test-explicit-model",{},server.key).result_int()==200);
    REQUIRE(server.call(http::verb::get,"/v1beta/models/test-explicit-model",{},server.key).result_int()==200);
    for(auto route:{"/api/accounts","/accounts","/config","/stats","/api-keys","/v1/responses","/"})REQUIRE(server.call(http::verb::get,route,{},server.key).result_int()==404);
    auto missing=server.call(http::verb::post,"/v1beta/models/test-explicit-model:generateContent",R"({"contents":[{"role":"user","parts":[{"text":"hello"}]}]})",server.key);REQUIRE(missing.result_int()==503);REQUIRE(missing.body().find("no_account")!=std::string::npos);
    auto malformed=server.call(http::verb::post,"/v1/chat/completions","{",server.key);REQUIRE(malformed.result_int()==400);
    auto large=server.call(http::verb::post,"/v1/chat/completions",std::string(5000,'x'),server.key);REQUIRE(large.result_int()==413);
    REQUIRE_FALSE(server.failure);
}

TEST_CASE("HTTP authorization follows the configured key across reloads","[http][config]"){
    LiveServer server;
    std::string legacy_key;
    {Store store(server.config.data_dir);legacy_key=string_or(store.create_key("legacy database key"),"secret");}
    REQUIRE(server.call(http::verb::get,"/v1/models",{},legacy_key).result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==200);
    auto shown=json::serialize(local_call(server.config,{{"command","config.show"}}));
    REQUIRE(shown.find(server.key)==std::string::npos);

    auto invalid=server.config.to_json();
    invalid["api_key"]="invalid key with whitespace";
    atomic_write(server.config.config_file,json::serialize(invalid),true);
    try {
        local_call(server.config,{{"command","config.reload"}});
        FAIL("Invalid key configuration was applied");
    } catch(const Error& error) {
        REQUIRE(error.code=="invalid_config");
    }
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==200);

    const auto old_key=server.key;
    server.key="rotated-http-test-"+random_hex(24);
    server.config.api_key=server.key;
    atomic_write(server.config.config_file,json::serialize(server.config.to_json()),true);
    REQUIRE(string_or(local_call(server.config,{{"command","config.reload"}}),"status")=="reloaded");
    REQUIRE(server.call(http::verb::get,"/v1/models",{},old_key).result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==200);
    shown=json::serialize(local_call(server.config,{{"command","config.show"}}));
    REQUIRE(shown.find(server.key)==std::string::npos);

    server.config.api_key.clear();
    atomic_write(server.config.config_file,json::serialize(server.config.to_json()),true);
    local_call(server.config,{{"command","config.reload"}});
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models",{},legacy_key).result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models").result_int()==401);
    const auto ready=server.call(http::verb::get,"/ready");
    REQUIRE(ready.result_int()==503);
    REQUIRE_FALSE(bool_or(json::parse(ready.body()).as_object(),"ready"));
}

TEST_CASE("An empty configured key never authorizes HTTP requests","[http][config]"){
    LiveServer server(false);
    REQUIRE(server.call(http::verb::get,"/health").result_int()==200);
    REQUIRE(server.call(http::verb::get,"/v1/models").result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==401);
    REQUIRE_FALSE(bool_or(local_call(server.config,{{"command","status"}}),"ready"));
}

TEST_CASE("HTTP listener reports an occupied port with actionable diagnostics","[http]"){
    net::io_context io;
    tcp::acceptor occupied(io);
    occupied.open(tcp::v4());
#ifdef _WIN32
    // Prevent the service's SO_REUSEADDR from sharing this Windows listener.
    const BOOL exclusive=TRUE;
    REQUIRE(::setsockopt(occupied.native_handle(),SOL_SOCKET,SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==0);
#endif
    occupied.bind({net::ip::make_address("127.0.0.1"),0});
    occupied.listen();
    struct TemporaryDirectory {
        fs::path path=fs::temp_directory_path()/("ais2api-occupied-http-"+random_hex(8));
        ~TemporaryDirectory(){std::error_code ec;fs::remove_all(path,ec);}
    } directory;
    Config config;
    config.data_dir=directory.path/"data";
    config.config_file=directory.path/"config.json";
    config.bind="127.0.0.1";
    config.port=occupied.local_endpoint().port();
    fs::create_directories(directory.path);
    try {
        serve(config);
        FAIL("Service unexpectedly started on an occupied HTTP port");
    } catch(const Error& error) {
        REQUIRE(error.status==503);
#ifdef _WIN32
        // An exclusive listener can produce WSAEACCES rather than WSAEADDRINUSE.
        REQUIRE((error.code=="http_permission_denied"||error.code=="http_address_in_use"));
#else
        REQUIRE(error.code=="http_address_in_use");
#endif
        const std::string message=error.what();
        REQUIRE(message.find("HTTP listener bind failed")!=std::string::npos);
        REQUIRE(message.find("bind=127.0.0.1")!=std::string::npos);
        REQUIRE(message.find("port="+std::to_string(config.port))!=std::string::npos);
        REQUIRE(message.find("reason="+error.code.substr(5))!=std::string::npos);
        REQUIRE(message.find("os_error=")!=std::string::npos);
        REQUIRE(message.find("different available port")!=std::string::npos);
    }
}
