// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include <catch2/catch_test_macros.hpp>
#include <future>
using namespace ais2api;
namespace {
struct RuntimeDirectory {
  fs::path path = fs::temp_directory_path() / ("ais2api-ipc-" + random_hex(8));
  RuntimeDirectory() { fs::create_directories(path); }
  ~RuntimeDirectory() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  Config config() {
    Config c;
    c.data_dir = path;
    c.config_file = path / "config.json";
    return c;
  }
};
} // namespace
TEST_CASE("instance lock excludes a second owner and releases on destruction") {
  RuntimeDirectory directory;
  {
    InstanceLock first(directory.path);
    try {
      InstanceLock second(directory.path);
      FAIL("Duplicate instance acquired lock");
    } catch (const Error &e) {
      CHECK(e.status == 409);
      CHECK(e.code == "already_running");
    }
  }
  CHECK_NOTHROW(InstanceLock(directory.path));
  CHECK(fs::is_regular_file(executable_path()));
}
TEST_CASE("local control performs framed JSON roundtrip without HTTP") {
  RuntimeDirectory directory;
  auto config = directory.config();
  ControlServer server(config, [](const json::object &command) {
    return json::object{{"echo", command}};
  });
  for (int i = 0; i < 8; ++i) {
    auto answer = local_call(
        config, {{"command", "stats"}, {"value", i}, {"unicode", "日本語"}});
    CHECK(answer.at("echo").as_object().at("value") == i);
    CHECK(answer.at("echo").as_object().at("unicode") == "日本語");
  }
}
TEST_CASE("local control preserves typed errors and hides unexpected exception "
          "contents") {
  RuntimeDirectory directory;
  auto config = directory.config();
  ControlServer server(config, [](const json::object &command) -> json::object {
    if (string_or(command, "command") == "typed")
      throw Error(409, "busy", "Operation is busy");
    throw std::runtime_error("secret internal credential");
  });
  try {
    local_call(config, {{"command", "typed"}});
    FAIL("Expected handler error");
  } catch (const Error &e) {
    CHECK(e.status == 409);
    CHECK(e.code == "busy");
    CHECK(std::string(e.what()) == "Operation is busy");
  }
  try {
    local_call(config, {{"command", "unexpected"}});
    FAIL("Expected handler error");
  } catch (const Error &e) {
    CHECK(e.status == 500);
    CHECK(std::string(e.what()).find("credential") == std::string::npos);
  }
}
TEST_CASE("local control refuses oversized requests and responses") {
  RuntimeDirectory directory;
  auto config = directory.config();
  ControlServer server(config, [](const json::object &) -> json::object {
    return {{"large", std::string(4 * 1024 * 1024, 'x')}};
  });
  try {
    local_call(config, {{"large", std::string(4 * 1024 * 1024, 'y')}});
    FAIL("Oversized request accepted");
  } catch (const Error &e) {
    CHECK(e.status == 413);
  }
  try {
    local_call(config, {{"small", true}});
    FAIL("Oversized response accepted");
  } catch (const Error &e) {
    CHECK(e.status == 413);
  }
}
TEST_CASE("control destructor cancels an idle listener promptly") {
  RuntimeDirectory directory;
  auto config = directory.config();
  auto start = Clock::now();
  {
    ControlServer server(config,
                         [](const json::object &) { return json::object{}; });
  }
  CHECK(Clock::now() - start < std::chrono::seconds(2));
}
TEST_CASE("local service absence has a distinct offline fallback error") {
  RuntimeDirectory directory;
  auto config = directory.config();
  try {
    local_call(config, {{"command", "stats"}});
    FAIL("Expected absent service");
  } catch (const Error &e) {
    CHECK(e.status == 503);
    CHECK(e.code == "not_running");
  }
}
TEST_CASE("local control endpoint cannot be replaced by a second server") {
  RuntimeDirectory directory;
  auto config = directory.config();
  ControlServer first(
      config, [](const json::object &) { return json::object{{"ok", true}}; });
  CHECK_THROWS_AS(
      ControlServer(config,
                    [](const json::object &) { return json::object{}; }),
      Error);
  CHECK(local_call(config, {}).at("ok").as_bool());
}
