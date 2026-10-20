// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/config.hpp"
#include <cstdlib>
#include <limits>

namespace ais2api {
namespace {
void merge(json::object &a, const json::object &b) {
  for (auto &v : b) {
    if (v.value().is_object() && a.if_contains(v.key()) &&
        a.at(v.key()).is_object())
      merge(a.at(v.key()).as_object(), v.value().as_object());
    else
      a[v.key()] = v.value();
  }
}
std::uint64_t number(const json::value &v) {
  if (v.is_uint64())
    return v.as_uint64();
  if (v.is_int64() && v.as_int64() >= 0)
    return static_cast<std::uint64_t>(v.as_int64());
  throw Error(400, "invalid_config", "Expected unsigned integer");
}
void fields(const json::object &o,
            std::initializer_list<std::string_view> allowed) {
  for (auto &p : o) {
    bool ok = false;
    for (auto k : allowed)
      if (p.key() == k)
        ok = true;
    if (!ok)
      throw Error(400, "invalid_config",
                  "Unknown configuration field: " + std::string(p.key()));
  }
}
bool env_exists(const char *key) {
#ifdef _WIN32
  std::wstring wide_key;
  while (*key)
    wide_key.push_back(static_cast<wchar_t>(*key++));
  return _wgetenv(wide_key.c_str()) != nullptr;
#else
  return std::getenv(key) != nullptr;
#endif
}
std::string path_text(const fs::path &path) {
  auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}
std::string env(const char *k) {
#ifdef _WIN32
  std::wstring key;
  while (*k)
    key.push_back(static_cast<wchar_t>(*k++));
  auto value = _wgetenv(key.c_str());
  return value ? path_text(fs::path(value)) : "";
#else
  auto p = std::getenv(k);
  return p ? p : "";
#endif
}
} // namespace
fs::path Config::default_directory() {
#ifdef _WIN32
  auto root = env("LOCALAPPDATA");
  if (root.empty())
    root = env("USERPROFILE");
#else
  auto root = env("XDG_CONFIG_HOME");
  if (root.empty()) {
    root = env("HOME");
    if (!root.empty())
      root += "/.config";
  }
#endif
  if (root.empty())
    throw Error(500, "environment",
                "Cannot determine user configuration directory");
  return fs::u8path(root) / "ais2api";
}
json::object Config::to_json() const {
  return {{"version", version},
          {"bind", bind},
          {"port", port},
          {"data_dir", path_text(data_dir)},
          {"browser_executable", path_text(browser_executable)},
          {"proxy", proxy},
          {"headless", headless},
          {"limits",
           {{"request_bytes", limits.request_bytes},
            {"object_bytes", limits.object_bytes},
            {"relay_bytes", limits.relay_bytes},
            {"queue_size", limits.queue_size},
            {"queue_seconds", limits.queue_seconds},
            {"generation_seconds", limits.generation_seconds},
            {"stream_idle_seconds", limits.stream_idle_seconds},
            {"max_attempts", limits.max_attempts}}},
          {"model_aliases", model_aliases},
          {"configured_models", configured_models},
          {"model_defaults", model_defaults}};
}
Config Config::load(const fs::path &file, const json::object &overrides) {
  Config c;
  c.config_file = file.empty() ? default_directory() / "config.json" : file;
  c.data_dir = default_directory() / "data";
  auto o = c.to_json();
  if (fs::exists(c.config_file)) {
    auto v = json::parse(read_file(c.config_file));
    if (!v.is_object())
      throw Error(400, "invalid_config", "Configuration must be an object");
    merge(o, v.as_object());
  }
  json::object e;
  for (auto pair : {std::pair{"AIS2API_BIND", "bind"},
                    {"AIS2API_DATA_DIR", "data_dir"},
                    {"AIS2API_BROWSER_EXECUTABLE", "browser_executable"},
                    {"AIS2API_PROXY", "proxy"}}) {
    auto v = env(pair.first);
    if (env_exists(pair.first))
      e[pair.second] = v;
  }
  for (auto pair :
       {std::pair{"AIS2API_PORT", "port"}, {"AIS2API_HEADLESS", "headless"}}) {
    auto v = env(pair.first);
    if (env_exists(pair.first)) {
      try {
        e[pair.second] = json::parse(v);
      } catch (...) {
        throw Error(400, "invalid_config", "Invalid environment value");
      }
    }
  }
  for (auto pair : {std::pair{"AIS2API_LIMITS", "limits"},
                    {"AIS2API_MODEL_ALIASES", "model_aliases"},
                    {"AIS2API_CONFIGURED_MODELS", "configured_models"},
                    {"AIS2API_MODEL_DEFAULTS", "model_defaults"}}) {
    auto value = env(pair.first);
    if (env_exists(pair.first)) {
      try {
        e[pair.second] = json::parse(value);
      } catch (...) {
        throw Error(400, "invalid_config", "Invalid JSON environment value");
      }
    }
  }
  json::object limit_env;
  for (auto pair : {std::pair{"AIS2API_REQUEST_BYTES", "request_bytes"},
                    {"AIS2API_OBJECT_BYTES", "object_bytes"},
                    {"AIS2API_RELAY_BYTES", "relay_bytes"},
                    {"AIS2API_QUEUE_SIZE", "queue_size"},
                    {"AIS2API_QUEUE_SECONDS", "queue_seconds"},
                    {"AIS2API_GENERATION_SECONDS", "generation_seconds"},
                    {"AIS2API_STREAM_IDLE_SECONDS", "stream_idle_seconds"},
                    {"AIS2API_MAX_ATTEMPTS", "max_attempts"}}) {
    auto value = env(pair.first);
    if (env_exists(pair.first)) {
      try {
        limit_env[pair.second] = json::parse(value);
      } catch (...) {
        throw Error(400, "invalid_config", "Invalid numeric environment value");
      }
    }
  }
  if (!limit_env.empty()) {
    if (!e.if_contains("limits"))
      e["limits"] = json::object{};
    if (!e.at("limits").is_object())
      throw Error(400, "invalid_config", "AIS2API_LIMITS must be an object");
    merge(e.at("limits").as_object(), limit_env);
  }
  merge(o, e);
  merge(o, overrides);
  fields(o, {"version", "bind", "port", "data_dir", "browser_executable",
             "proxy", "headless", "limits", "model_aliases",
             "configured_models", "model_defaults"});
  auto text = [&](const char *k) {
    if (!o.at(k).is_string())
      throw Error(400, "invalid_config", "Expected string: " + std::string(k));
    return std::string(o.at(k).as_string());
  };
  if (number(o.at("version")) != 1)
    throw Error(400, "invalid_config", "Unsupported configuration version");
  auto port = number(o.at("port"));
  if (port == 0 || port > 65535)
    throw Error(400, "invalid_config", "Port outside 1..65535");
  c.port = static_cast<unsigned short>(port);
  c.bind = text("bind");
  c.data_dir = fs::u8path(text("data_dir"));
  c.browser_executable = fs::u8path(text("browser_executable"));
  c.proxy = text("proxy");
  if (!o.at("headless").is_bool())
    throw Error(400, "invalid_config", "headless must be boolean");
  c.headless = o.at("headless").as_bool();
  if (!o.at("limits").is_object() || !o.at("model_aliases").is_object() ||
      !o.at("configured_models").is_array() ||
      !o.at("model_defaults").is_object())
    throw Error(400, "invalid_config", "Invalid configuration container");
  auto &l = o.at("limits").as_object();
  fields(l, {"request_bytes", "object_bytes", "relay_bytes", "queue_size",
             "queue_seconds", "generation_seconds", "stream_idle_seconds",
             "max_attempts"});
  auto n = [&](const char *k) {
    auto x = number(l.at(k));
    if (!x || x > std::numeric_limits<unsigned>::max())
      throw Error(400, "invalid_config", "Limit outside supported range");
    return static_cast<unsigned>(x);
  };
  c.limits.request_bytes = n("request_bytes");
  c.limits.object_bytes = n("object_bytes");
  c.limits.relay_bytes = n("relay_bytes");
  c.limits.queue_size = n("queue_size");
  c.limits.queue_seconds = n("queue_seconds");
  c.limits.generation_seconds = n("generation_seconds");
  c.limits.stream_idle_seconds = n("stream_idle_seconds");
  c.limits.max_attempts = n("max_attempts");
  c.model_aliases = o.at("model_aliases").as_object();
  c.configured_models = o.at("configured_models").as_array();
  c.model_defaults = o.at("model_defaults").as_object();
  c.validate();
  return c;
}
void Config::validate() const {
  if (bind.empty() || data_dir.empty() || !port)
    throw Error(400, "invalid_config",
                "Bind, data directory and port are required");
  for (auto &p : model_aliases)
    if (!p.value().is_string() || p.value().as_string().empty())
      throw Error(400, "invalid_config",
                  "Model aliases must map to nonempty strings");
  for (auto &m : configured_models)
    if (!m.is_string() && !m.is_object())
      throw Error(400, "invalid_config", "Invalid configured model");
  if (!limits.request_bytes || !limits.object_bytes || !limits.relay_bytes ||
      !limits.queue_size || !limits.queue_seconds ||
      !limits.generation_seconds || !limits.stream_idle_seconds ||
      !limits.max_attempts || limits.max_attempts > 16 ||
      limits.queue_size > 4096 || limits.relay_bytes > limits.object_bytes)
    throw Error(400, "invalid_config", "Inconsistent resource limits");
}
} // namespace ais2api
