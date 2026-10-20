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
    LiveServer(){
        net::io_context io;tcp::acceptor port(io,{tcp::v4(),0});auto free_port=port.local_endpoint().port();port.close();
        config.data_dir=directory/"data";config.config_file=directory/"config.json";config.port=free_port;config.configured_models={"test-explicit-model"};config.limits.request_bytes=4096;config.limits.object_bytes=8192;config.limits.relay_bytes=1024;
        fs::create_directories(directory);atomic_write(config.config_file,json::serialize(config.to_json()),true);
        {Store store(config.data_dir);key=string_or(store.create_key("test"),"secret");}
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
