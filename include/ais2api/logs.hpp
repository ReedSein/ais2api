// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "ais2api/common.hpp"
#include <mutex>
#include <vector>

namespace ais2api::logs {
// Only pass deliberately selected, non-secret fields to this journal.
// Cursors are byte offsets across rotations: a rotated file starts with a
// journal_rotated record whose base is the offset of its first byte.
class Journal {
  public:
    static constexpr std::size_t max_record_bytes = 64 * 1024;
    static constexpr std::uint64_t default_rotate_bytes = 8 * 1024 * 1024;
    explicit Journal(fs::path data_dir, std::uint64_t rotate_bytes = default_rotate_bytes);
    void append(json::object record);
    // Writes the records with one flush; oversized records are skipped and
    // reported by throwing after the others were persisted.
    void append(std::vector<json::object> records);
    json::object read(std::string_view cursor = "0", std::size_t limit = 100) const;
  private:
    fs::path file_;
    std::uint64_t rotate_bytes_;
    // Size at which a failed rotation is attempted again.
    std::uint64_t rotate_retry_ = 0, mirror_retry_ = 0;
    // service.log when it receives this process's stderr, else empty.
    fs::path mirror_;
    mutable std::mutex mutex_;
    void trim_mirror();
};
} // namespace ais2api::logs
