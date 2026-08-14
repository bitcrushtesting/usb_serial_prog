// SPDX-License-Identifier: GPL-2.0-only
//
// usb_serial_prog - configuration EEPROM programmer for USB serial chips
// Copyright (C) 2026 Bitcrush Testing
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License version 2 as published by
// the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace usbprog::cli {

/// Classic 16-bytes-per-line hex dump with an ASCII column.
void hexdump(std::ostream& out, std::span<const uint8_t> data, std::size_t baseAddress = 0);

std::string padRight(std::string text, std::size_t width);

/// Print "key   value" pairs with the values aligned.
void printTable(std::ostream& out, const std::vector<std::pair<std::string, std::string>>& rows,
                const std::string& indent = "  ");

/// Ask the user to confirm a destructive action. Returns false when they
/// decline. Throws when stdin is not a terminal and --yes was not given, so
/// scripts fail loudly instead of hanging.
bool confirm(const std::string& question, bool assumeYes);

std::vector<uint8_t> readBinaryFile(const std::string& path);
void writeBinaryFile(const std::string& path, std::span<const uint8_t> data);

/// Timestamped file name used when no explicit backup path was given.
std::string defaultBackupName(uint16_t vendorId, uint16_t productId);

} // namespace usbprog::cli
