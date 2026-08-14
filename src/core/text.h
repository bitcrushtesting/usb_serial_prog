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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace usbprog::text {

/// Convert UTF-8 (as it arrives from the command line) to UTF-16LE code units,
/// which is what USB string descriptors store. Throws Error on invalid input.
std::vector<uint16_t> toUtf16(std::string_view utf8);

/// Convert UTF-16 code units back to UTF-8. Lone surrogates are replaced by
/// U+FFFD instead of throwing, so a garbled EEPROM still prints something.
std::string toUtf8(std::span<const uint16_t> utf16);

/// Number of Unicode code points, i.e. how many UTF-16 code units a string
/// needs (surrogate pairs count as two).
std::size_t utf16Length(std::string_view utf8);

std::string toLower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
std::string trim(std::string_view s);

/// Parse an unsigned integer in decimal, 0x-hex or 0b-binary notation. Returns
/// nothing when the text is not a number or does not fit into `maximum`.
std::optional<uint32_t> tryParseUnsigned(std::string_view s, uint32_t maximum);

/// The same, but with an Error explaining what is wrong with the input.
uint32_t parseUnsigned(std::string_view s, uint32_t maximum);

/// Parse "VID:PID" with hexadecimal components, e.g. "0403:6001".
void parseVidPid(std::string_view s, uint16_t& vid, uint16_t& pid);

std::string hex16(uint16_t value);
std::string hex8(uint8_t value);

} // namespace usbprog::text
