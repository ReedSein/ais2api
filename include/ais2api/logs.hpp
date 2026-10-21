// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "ais2api/common.hpp"
#include <mutex>

namespace ais2api::logs {
// Only pass deliberately selected, non-secret fields to this journal.
class Journal {
  public:
    static constexpr std::size_t max_record_bytes = 64 * 1024;
    explicit Journal(fs::path data_dir);
    void append(json::object record);
    json::object read(std::string_view cursor = "0", std::size_t limit = 100) const;
  private:
    fs::path file_;
    mutable std::mutex mutex_;
};
} // namespace ais2api::logs
