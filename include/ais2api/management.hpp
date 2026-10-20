// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "runtime.hpp"
#include "store.hpp"
namespace ais2api {
json::object manage(Store& store, Config& config, const json::object& command);
}
