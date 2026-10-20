// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "common.hpp"
#include <array>
#include <condition_variable>
#include <mutex>
namespace ais2api {
// One decoded object may be larger than the relay window (e.g. an atomic image).
// It has its own object limit; serialized output is written in relay-sized pieces.
class EventPipe {
  public:
    explicit EventPipe(std::size_t object_limit) : object_limit_(object_limit) {}
    std::stop_source cancellation;
    std::atomic<bool> started{false};
    std::atomic<std::uint64_t> produced_bytes{0};
    // Install once before publishing the pipe to a producer. The callback must
    // only schedule a consumer wakeup; it must not synchronously enter the pipe.
    void set_wakeup(std::function<void()> callback) {
        std::lock_guard lock(mutex_);
        wakeup_ = std::move(callback);
    }
    void disable_wakeup() {
        std::lock_guard lock(mutex_);
        wakeup_ = {};
    }
    void push(json::object value, Clock::time_point deadline) {
        json::serializer serializer;
        serializer.reset(&value);
        std::array<char, 16384> bytes{};
        std::size_t size = 0;
        while (!serializer.done()) {
            auto part = serializer.read(bytes.data(), bytes.size());
            if (part.size() > object_limit_ - size)
                throw Error(413, "object_limit", "Decoded response exceeds object limit");
            size += part.size();
        }
        std::unique_lock lock(mutex_);
        auto wait = [&](auto condition) {
            while (!condition()) {
                check_cancel(cancellation.get_token());
                if (Clock::now() >= deadline)
                    throw Error(504, "consumer_timeout", "Downstream consumer timed out");
                changed_.wait_for(lock, std::chrono::milliseconds(20));
            }
        };
        wait([&] { return !pending_; });
        pending_ = std::move(value);
        produced_bytes.fetch_add(size);
        changed_.notify_all();
        if (wakeup_)
            wakeup_();
        wait([&] { return !pending_; });
    }
    std::optional<json::object> take() {
        std::lock_guard lock(mutex_);
        if (!pending_ || taken_)
            return {};
        taken_ = true;
        return std::move(*pending_);
    }
    void acknowledge() {
        std::lock_guard lock(mutex_);
        pending_.reset();
        taken_ = false;
        changed_.notify_all();
    }
    void finish(std::optional<Error> error = {}) {
        std::lock_guard lock(mutex_);
        error_ = std::move(error);
        done_ = true;
        changed_.notify_all();
        if (wakeup_)
            wakeup_();
    }
    bool done() {
        std::lock_guard lock(mutex_);
        return done_ && !pending_;
    }
    std::optional<Error> error() {
        std::lock_guard lock(mutex_);
        return error_;
    }
    void cancel() {
        cancellation.request_stop();
        std::lock_guard lock(mutex_);
        changed_.notify_all();
        if (wakeup_)
            wakeup_();
    }

  private:
    std::size_t object_limit_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<json::object> pending_;
    std::optional<Error> error_;
    std::function<void()> wakeup_;
    bool taken_ = false, done_ = false;
};
} // namespace ais2api
