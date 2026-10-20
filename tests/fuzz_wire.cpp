// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/protocol.hpp"
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size){
    if(size>1024*1024)return 0;
    try{ais2api::WireStreamParser parser(65536);std::string_view input(reinterpret_cast<const char*>(data),size);while(!input.empty()){auto n=std::min<std::size_t>(input.size(),1+(static_cast<unsigned char>(input.front())%64));parser.feed(input.substr(0,n),[](ais2api::json::value value){try{(void)ais2api::decode_chunk(value);}catch(const std::exception&) {}});input.remove_prefix(n);}parser.finish();}catch(const std::exception&){}
    return 0;
}
