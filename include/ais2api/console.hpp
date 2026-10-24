// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "common.hpp"
namespace ais2api::console {
enum class Stream { out, err };
enum class Format { json, text };
// AIS2API_CONSOLE_FORMAT is auto (default), text or json. Auto selects text
// only for an interactive console; files and pipes keep single-line JSON.
Format choose(std::string_view setting, bool interactive);
Format format(Stream stream);
// Writes UTF-8 text, using wide console output on Windows consoles.
void write(Stream stream, std::string_view utf8);
// A readable, newline-terminated line for a journal record; empty for
// routine successful HTTP requests, which the journal still keeps.
std::string log_line(const json::object& record, bool color = false);
// Indented JSON for people reading command results.
std::string pretty(const json::value& value);
// Journal and setup records on stderr, in the selected format.
void emit_records(const std::vector<json::object>& records, std::string_view json_lines);
void emit_record(const json::object& record);
// Command results on stdout and command errors on stderr.
void emit_result(const json::value& result);
void emit_error(const json::object& error);
} // namespace ais2api::console
