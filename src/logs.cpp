// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/logs.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <syncstream>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ais2api::logs {
namespace {
void inspect(const fs::path &file) {
    for (auto parent = file.parent_path(); !parent.empty();) {
        if (fs::is_symlink(fs::symlink_status(parent)))
            throw Error(500, "log_path", "Journal directory cannot be a symbolic link");
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    const auto status = fs::symlink_status(file);
    if (fs::is_symlink(status) || (fs::exists(status) && !fs::is_regular_file(status)))
        throw Error(500, "log_path", "Journal must be a regular file");
}
void write_line(const fs::path &file, const std::string &line) {
    inspect(file);
#ifdef _WIN32
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE | WRITE_DAC, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw Error(500, "log_write", "Cannot open journal");
    struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } guard{handle};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw Error(500, "log_path", "Journal must be a regular file");
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto parent = file.parent_path();
    auto result = GetNamedSecurityInfoW(const_cast<LPWSTR>(parent.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, &acl, nullptr, &descriptor);
    if (result == ERROR_SUCCESS)
        result = SetSecurityInfo(handle, SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                 nullptr, nullptr, acl, nullptr);
    if (descriptor) LocalFree(descriptor);
    if (result != ERROR_SUCCESS)
        throw Error(500, "log_write", "Cannot protect journal");
    LARGE_INTEGER end{};
    if (!SetFilePointerEx(handle, end, nullptr, FILE_END))
        throw Error(500, "log_write", "Cannot seek journal");
    DWORD written = 0;
    if (!WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
        written != line.size() || !FlushFileBuffers(handle))
        throw Error(500, "log_write", "Cannot persist journal entry");
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd < 0) throw Error(500, "log_write", "Cannot open journal");
    struct Guard { int fd; ~Guard() { ::close(fd); } } guard{fd};
    struct stat status{};
    if (::fstat(fd, &status) || !S_ISREG(status.st_mode) || ::fchmod(fd, 0600))
        throw Error(500, "log_write", "Cannot protect journal");
    std::size_t offset = 0;
    while (offset < line.size()) {
        auto count = ::write(fd, line.data() + offset, line.size() - offset);
        if (count <= 0) throw Error(500, "log_write", "Cannot persist journal entry");
        offset += static_cast<std::size_t>(count);
    }
    if (::fsync(fd)) throw Error(500, "log_write", "Cannot flush journal");
#endif
}
}
Journal::Journal(fs::path data_dir) : file_(fs::absolute(data_dir) / "management.log") {
    inspect(file_);
    secure_directory(file_.parent_path());
    write_line(file_, "");
}
void Journal::append(json::object record) {
    if (!record.contains("timestamp")) record["timestamp"] = unix_seconds();
    if (!record.contains("id")) record["id"] = random_hex(12);
    auto line = json::serialize(record) + '\n';
    if (line.size() > max_record_bytes)
        throw Error(413, "log_limit", "Journal entry exceeds size limit");
    std::lock_guard lock(mutex_);
    // Emit once even when persistence fails; the caller still receives the error.
    std::osyncstream(std::cerr) << line << std::flush;
    inspect(file_);
    std::ifstream existing(file_, std::ios::binary | std::ios::ate);
    if (!existing) throw Error(500, "log_read", "Cannot inspect journal before appending");
    auto length = existing.tellg();
    if (length < 0) throw Error(500, "log_read", "Cannot inspect journal before appending");
    if (length > 0) {
        existing.seekg(-1, std::ios::end);
        if (existing.get() != '\n')
            throw Error(500, "log_record", "Journal has an incomplete final entry");
    }
    existing.close();
    write_line(file_, line);
}
json::object Journal::read(std::string_view cursor, std::size_t limit) const {
    std::uint64_t offset = 0;
    const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
    if (cursor.empty() || parsed.ec != std::errc{} || parsed.ptr != cursor.data() + cursor.size() ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        limit == 0 || limit > 500)
        throw Error(400, "invalid_cursor", "Invalid journal cursor or limit");
    // Readers observe complete lines only and may retry an unfinished tail.
    // Do not wait on a writer doing fsync or a backpressured stderr write.
    inspect(file_);
    std::ifstream in(file_, std::ios::binary);
    if (!in) throw Error(500, "log_read", "Cannot read journal");
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) throw Error(500, "log_read", "Cannot inspect journal");
    if (offset > static_cast<std::uint64_t>(size))
        throw Error(400, "invalid_cursor", "Journal cursor is beyond end of file");
    if (offset) {
        in.seekg(static_cast<std::streamoff>(offset - 1));
        if (in.get() != '\n')
            throw Error(400, "invalid_cursor", "Journal cursor is not a line boundary");
    }
    in.seekg(static_cast<std::streamoff>(offset));
    json::array entries;
    auto next = offset;
    std::size_t batch_bytes = 0;
    bool more = false;
    while (in.peek() != std::char_traits<char>::eof()) {
        std::string line;
        char c = 0;
        bool complete = false;
        while (in.get(c)) {
            if (c == '\n') { complete = true; break; }
            if (line.size() >= max_record_bytes - 1)
                throw Error(500, "log_record", "Journal entry exceeds size limit");
            line += c;
        }
        if (!complete) break;
        if (entries.size() == limit ||
            (!entries.empty() && batch_bytes + line.size() + 1 > 256 * 1024)) {
            more = true;
            break;
        }
        boost::system::error_code error;
        auto record = json::parse(line, error);
        if (error || !record.is_object())
            throw Error(500, "log_record", "Journal contains an invalid entry");
        entries.emplace_back(json::object{{"cursor", std::to_string(next)}, {"record", std::move(record)}});
        next += line.size() + 1;
        batch_bytes += line.size() + 1;
    }
    if (in.bad()) throw Error(500, "log_read", "Cannot read journal");
    return {{"entries", std::move(entries)}, {"next_cursor", std::to_string(next)}, {"has_more", more}};
}
} // namespace ais2api::logs
