// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include <array>
#include <cstring>
#include <future>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
namespace ais2api {
namespace {
constexpr std::size_t frame_limit = 4 * 1024 * 1024;
constexpr auto io_timeout = std::chrono::seconds(5);
#ifdef _WIN32
using Native = HANDLE;
const Native invalid = INVALID_HANDLE_VALUE;
void close_native(Native h) {
  if (h != invalid && h != nullptr)
    CloseHandle(h);
}
std::wstring current_sid() {
  HANDLE token{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    throw Error(500, "permissions", "Cannot inspect current user");
  DWORD n = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &n);
  std::vector<unsigned char> bytes(n);
  if (!GetTokenInformation(token, TokenUser, bytes.data(), n, &n)) {
    CloseHandle(token);
    throw Error(500, "permissions", "Cannot inspect current user");
  }
  CloseHandle(token);
  LPWSTR sid{};
  if (!ConvertSidToStringSidW(
          reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid))
    throw Error(500, "permissions", "Cannot inspect current SID");
  std::wstring out = sid;
  LocalFree(sid);
  return out;
}
std::wstring endpoint(const Config &config) {
  auto canonical = fs::weakly_canonical(config.data_dir).native();
  CharLowerBuffW(canonical.data(), static_cast<DWORD>(canonical.size()));
  auto path = fs::path(canonical).u8string();
  auto hash = sha256(
      std::string(reinterpret_cast<const char *>(path.data()), path.size()));
  return L"\\\\.\\pipe\\ais2api-" + current_sid() + L"-" +
         std::wstring(hash.begin(), hash.end());
}
bool same_pipe_server(Native pipe) {
  ULONG pid = 0;
  if (!GetNamedPipeServerProcessId(pipe, &pid))
    return false;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return false;
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
    CloseHandle(process);
    return false;
  }
  CloseHandle(process);
  DWORD n = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &n);
  std::vector<unsigned char> bytes(n);
  if (!GetTokenInformation(token, TokenUser, bytes.data(), n, &n)) {
    CloseHandle(token);
    return false;
  }
  CloseHandle(token);
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(
          reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid))
    return false;
  bool result = std::wstring(sid) == current_sid();
  LocalFree(sid);
  return result;
}
struct Event {
  HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Event() {
    if (!handle)
      throw Error(500, "control_io", "Cannot create control event");
  }
  ~Event() { CloseHandle(handle); }
};
DWORD complete(Native pipe, OVERLAPPED &operation, Clock::time_point deadline,
               std::stop_token stop) {
  for (;;) {
    if (stop.stop_requested() || Clock::now() >= deadline) {
      CancelIoEx(pipe, &operation);
      DWORD ignored{};
      GetOverlappedResult(pipe, &operation, &ignored, TRUE);
      throw Error(503, "control_timeout", "Local control operation timed out");
    }
    auto wait = WaitForSingleObject(operation.hEvent, 50);
    if (wait == WAIT_OBJECT_0) {
      DWORD bytes = 0;
      if (!GetOverlappedResult(pipe, &operation, &bytes, FALSE))
        throw Error(503, "control_io", "Local control connection closed");
      return bytes;
    }
    if (wait != WAIT_TIMEOUT)
      throw Error(503, "control_io", "Cannot wait for local control operation");
  }
}
void transfer(Native pipe, void *bytes, std::size_t size, bool writing,
              Clock::time_point deadline, std::stop_token stop) {
  auto data = static_cast<unsigned char *>(bytes);
  while (size) {
    if (stop.stop_requested())
      throw Error(503, "control_stopped", "Local control server stopped");
    if (Clock::now() >= deadline)
      throw Error(503, "control_timeout", "Local control operation timed out");
    Event event;
    OVERLAPPED operation{};
    operation.hEvent = event.handle;
    DWORD done = 0;
    BOOL ok =
        writing
            ? WriteFile(pipe, data, static_cast<DWORD>(size), &done, &operation)
            : ReadFile(pipe, data, static_cast<DWORD>(size), &done, &operation);
    if (!ok) {
      if (GetLastError() != ERROR_IO_PENDING)
        throw Error(503, "control_io", "Local control connection closed");
      done = complete(pipe, operation, deadline, stop);
    }
    if (!done)
      throw Error(503, "control_io", "Local control connection closed");
    data += done;
    size -= done;
  }
}
#else
using Native = int;
constexpr Native invalid = -1;
void close_native(Native fd) {
  if (fd >= 0)
    ::close(fd);
}
fs::path endpoint(const Config &config) {
  return config.data_dir / "control.sock";
}
sockaddr_un address(const Config &config) {
  sockaddr_un out{};
  out.sun_family = AF_UNIX;
  auto path = endpoint(config).string();
  if (path.size() >= sizeof(out.sun_path))
    throw Error(
        400, "control_path",
        "Control socket path is too long; use a shorter data directory");
  std::memcpy(out.sun_path, path.c_str(), path.size() + 1);
  return out;
}
void ready(Native fd, short events, Clock::time_point deadline,
           std::stop_token stop) {
  for (;;) {
    if (stop.stop_requested())
      throw Error(503, "control_stopped", "Local control server stopped");
    if (Clock::now() >= deadline)
      throw Error(503, "control_timeout", "Local control operation timed out");
    pollfd p{fd, events, 0};
    int rc = ::poll(&p, 1, 50);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      throw Error(503, "control_io", "Local control poll failed");
    }
    if (rc > 0) {
      if (p.revents & events)
        return;
      if (p.revents & (POLLHUP | POLLERR | POLLNVAL))
        throw Error(503, "control_io", "Local control connection closed");
    }
  }
}
void transfer(Native fd, void *bytes, std::size_t size, bool writing,
              Clock::time_point deadline, std::stop_token stop) {
  auto data = static_cast<unsigned char *>(bytes);
  while (size) {
    ready(fd, writing ? POLLOUT : POLLIN, deadline, stop);
    auto n = writing ? ::send(fd, data, size, MSG_NOSIGNAL)
                     : ::recv(fd, data, size, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (n <= 0)
      throw Error(503, "control_io", "Local control connection closed");
    data += n;
    size -= static_cast<std::size_t>(n);
  }
}
void nonblock(Native fd) {
  auto flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw Error(500, "control_io", "Cannot configure control socket");
  }
}
bool same_user(Native fd) {
  struct ucred peer{};
  socklen_t size = sizeof(peer);
  return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
         peer.uid == ::geteuid();
}
#endif
struct Connection {
  Native handle = invalid;
  explicit Connection(Native value) : handle(value) {}
  ~Connection() { close_native(handle); }
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
};
void send_frame(Native h, const json::object &message,
                Clock::time_point deadline, std::stop_token stop = {}) {
  auto data = json::serialize(message);
  if (data.size() > frame_limit)
    throw Error(413, "control_frame_too_large",
                "Local control frame exceeds 4 MiB");
  auto size = static_cast<std::uint32_t>(data.size());
  std::array<unsigned char, 4> header{static_cast<unsigned char>(size),
                                      static_cast<unsigned char>(size >> 8),
                                      static_cast<unsigned char>(size >> 16),
                                      static_cast<unsigned char>(size >> 24)};
  transfer(h, header.data(), header.size(), true, deadline, stop);
  transfer(h, data.data(), data.size(), true, deadline, stop);
}
json::object read_frame(Native h, Clock::time_point deadline,
                        std::stop_token stop = {}) {
  std::array<unsigned char, 4> header{};
  transfer(h, header.data(), header.size(), false, deadline, stop);
  auto size = static_cast<std::uint32_t>(header[0]) |
              (static_cast<std::uint32_t>(header[1]) << 8) |
              (static_cast<std::uint32_t>(header[2]) << 16) |
              (static_cast<std::uint32_t>(header[3]) << 24);
  if (size == 0 || size > frame_limit)
    throw Error(413, "control_frame_too_large",
                "Local control frame must contain 1 byte to 4 MiB");
  std::string data(size, '\0');
  transfer(h, data.data(), data.size(), false, deadline, stop);
  boost::system::error_code ec;
  auto value = json::parse(data, ec);
  if (ec || !value.is_object())
    throw Error(400, "invalid_control_json",
                "Local control message must be a UTF-8 JSON object");
  return value.as_object();
}
json::object response_error(const Error &error) {
  return {{"__error",
           {{"status", error.status},
            {"code", error.code},
            {"message", std::string(error.what())}}}};
}
} // namespace
struct ControlServer::Impl {
  Config config;
  Handler handler;
  std::jthread worker;
#ifndef _WIN32
  Native listener = invalid;
#endif
  Impl(const Config &c, Handler h) : config(c), handler(std::move(h)) {
    secure_directory(config.data_dir);
#ifndef _WIN32
    auto addr = address(config);
    auto path = endpoint(config);
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
      if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid())
        throw Error(
            409, "unsafe_control_path",
            "Existing control endpoint has unexpected ownership or type");
      Connection probe(
          ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
      if (probe.handle < 0)
        throw Error(500, "control_io",
                    "Cannot inspect existing control socket");
      int connected =
          probe.handle < 0
              ? -1
              : ::connect(probe.handle, reinterpret_cast<sockaddr *>(&addr),
                          sizeof(addr));
      if (connected == 0 ||
          (probe.handle >= 0 && (errno == EINPROGRESS || errno == EAGAIN)))
        throw Error(409, "already_running",
                    "Control endpoint is already in use");
      if (errno != ECONNREFUSED && errno != ENOENT)
        throw Error(500, "control_io",
                    "Cannot inspect existing control endpoint");
      if (::unlink(path.c_str()) != 0)
        throw Error(500, "control_io", "Cannot remove stale control socket");
    }
    listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
      throw Error(500, "control_io", "Cannot create control socket");
    try {
      nonblock(listener);
      if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
              0 ||
          ::chmod(path.c_str(), 0600) != 0 || ::listen(listener, 8) != 0)
        throw Error(500, "control_io", "Cannot bind control socket");
    } catch (...) {
      close_native(listener);
      listener = invalid;
      throw;
    }
#endif
    std::promise<void> started;
    auto future = started.get_future();
    worker = std::jthread([this, &started](std::stop_token stop) {
#ifdef _WIN32
      bool first = true;
      for (;;) {
        if (stop.stop_requested())
          break;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        try {
          auto sddl = L"D:P(A;;GA;;;" + current_sid() + L")";
          if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                  sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            throw Error(500, "permissions", "Cannot create named pipe ACL");
          SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),
                                         descriptor, FALSE};
          Connection pipe(
              CreateNamedPipeW(endpoint(config).c_str(),
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                   FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                   PIPE_REJECT_REMOTE_CLIENTS,
                               1, 65536, 65536, 0, &attributes));
          LocalFree(descriptor);
          descriptor = nullptr;
          if (pipe.handle == invalid)
            throw Error(409, "control_io", "Cannot create local control pipe");
          if (first) {
            started.set_value();
            first = false;
          }
          Event event;
          OVERLAPPED operation{};
          operation.hEvent = event.handle;
          BOOL connected = ConnectNamedPipe(pipe.handle, &operation);
          if (!connected) {
            auto error = GetLastError();
            if (error == ERROR_IO_PENDING)
              complete(pipe.handle, operation, Clock::time_point::max(), stop);
            else if (error != ERROR_PIPE_CONNECTED)
              throw Error(503, "control_io", "Cannot accept control client");
          }
          if (!stop.stop_requested())
            process(pipe.handle, stop);
          DisconnectNamedPipe(pipe.handle);
        } catch (...) {
          if (descriptor)
            LocalFree(descriptor);
          if (first) {
            started.set_exception(std::current_exception());
            return;
          }
          if (stop.stop_requested())
            return;
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      }
#else
      started.set_value();
      while (!stop.stop_requested()) {
        try {
          ready(listener, POLLIN, Clock::time_point::max(), stop);
          Connection client(::accept4(listener, nullptr, nullptr,
                                      SOCK_CLOEXEC | SOCK_NONBLOCK));
          if (client.handle < 0)
            continue;
          if (!same_user(client.handle))
            continue;
          process(client.handle, stop);
        } catch (...) {
          if (stop.stop_requested())
            break;
        }
      }
#endif
    });
    try {
      future.get();
    } catch (...) {
      worker.request_stop();
      if (worker.joinable())
        worker.join();
#ifndef _WIN32
      close_native(listener);
      listener = invalid;
      ::unlink(endpoint(config).c_str());
#endif
      throw;
    }
  }
  void process(Native h, std::stop_token stop) {
    auto deadline = Clock::now() + io_timeout;
    json::object response;
    try {
      auto request = read_frame(h, deadline, stop);
      response = handler(request);
    } catch (const Error &e) {
      response = response_error(e);
    } catch (const std::exception &) {
      response = response_error(
          Error(500, "control_handler", "Local control handler failed"));
    }
    try {
      if (json::serialize(response).size() > frame_limit)
        response =
            response_error(Error(413, "control_frame_too_large",
                                 "Local control response exceeds 4 MiB"));
      send_frame(h, response, Clock::now() + io_timeout, stop);
#ifdef _WIN32
      unsigned char ignored{};
      transfer(h, &ignored, 1, false, Clock::now() + io_timeout, stop);
#endif
    } catch (...) {
    }
  }
  ~Impl() {
    worker.request_stop();
    if (worker.joinable())
      worker.join();
#ifndef _WIN32
    close_native(listener);
    ::unlink(endpoint(config).c_str());
#endif
  }
};
ControlServer::ControlServer(const Config &c, Handler h)
    : impl_(std::make_unique<Impl>(c, std::move(h))) {}
ControlServer::~ControlServer() = default;
json::object local_call(const Config &config, const json::object &command) {
  auto deadline = Clock::now() + io_timeout;
#ifdef _WIN32
  auto name = endpoint(config);
  HANDLE pipe = invalid;
  for (;;) {
    pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != invalid)
      break;
    auto code = GetLastError();
    if ((code != ERROR_PIPE_BUSY && code != ERROR_FILE_NOT_FOUND) ||
        Clock::now() >= deadline)
      throw Error(503, "not_running", "Local service is not running");
    if (code == ERROR_FILE_NOT_FOUND)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    else
      WaitNamedPipeW(name.c_str(), 50);
  }
  Connection connection(pipe);
  if (!same_pipe_server(pipe))
    throw Error(403, "control_peer",
                "Local service belongs to a different user");
#else
  Connection connection(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (connection.handle < 0)
    throw Error(503, "not_running", "Cannot open local control socket");
  nonblock(connection.handle);
  auto addr = address(config);
  if (::connect(connection.handle, reinterpret_cast<sockaddr *>(&addr),
                sizeof(addr)) != 0) {
    if (errno != EINPROGRESS)
      throw Error(503, "not_running", "Local service is not running");
    try {
      ready(connection.handle, POLLOUT, deadline, {});
      int error = 0;
      socklen_t size = sizeof(error);
      if (::getsockopt(connection.handle, SOL_SOCKET, SO_ERROR, &error,
                       &size) != 0 ||
          error)
        throw Error(503, "not_running", "Local service is not running");
    } catch (...) {
      throw Error(503, "not_running", "Local service is not running");
    }
  }
  if (!same_user(connection.handle))
    throw Error(403, "control_peer",
                "Local service belongs to a different user");
#endif
  send_frame(connection.handle, command, deadline);
  auto response = read_frame(connection.handle, deadline);
  if (auto p = response.if_contains("__error"); p && p->is_object()) {
    auto &e = p->as_object();
    int status = 500;
    if (auto s = e.if_contains("status"); s && s->is_int64())
      status = static_cast<int>(s->as_int64());
    throw Error(status, string_or(e, "code", "control_error"),
                string_or(e, "message", "Local control operation failed"));
  }
  return response;
}
} // namespace ais2api
