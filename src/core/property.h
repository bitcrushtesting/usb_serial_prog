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
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace usbprog {

enum class PropertyType {
    Bool,   ///< on/off, yes/no, true/false, 1/0
    Number, ///< unsigned integer, decimal or 0x-hex
    String, ///< UTF-8 text stored as a USB string descriptor
    Enum,   ///< one of a fixed set of named values
};

struct EnumValue {
    std::string name;
    uint32_t value = 0;
    std::string help;
};

/// Describes one configuration item a chip exposes. Drivers publish a list of
/// these so the CLI can parse, validate and print values without knowing
/// anything about the chip itself.
struct PropertySpec {
    std::string name;
    PropertyType type = PropertyType::String;
    std::string help;
    std::string unit;         ///< printed after Number values, e.g. "mA"
    bool writable = true;     ///< false for things like the chip revision
    bool hexadecimal = false; ///< print Number values as 0x....

    uint32_t minimum = 0;      ///< Number only
    uint32_t maximum = 0xFFFF; ///< Number only
    uint32_t step = 1;         ///< Number only, value must be a multiple

    std::size_t maxChars = 0; ///< String only, 0 = limited by EEPROM space

    std::vector<EnumValue> values; ///< Enum only

    const EnumValue* find(std::string_view wanted) const;
    const EnumValue* find(uint32_t wanted) const;
};

using PropertyValue = std::variant<bool, uint32_t, std::string>;
using PropertyMap = std::map<std::string, PropertyValue, std::less<>>;

/// Render a value the way the CLI prints it (and the way `set` accepts it back).
std::string format(const PropertySpec& spec, const PropertyValue& value);

/// Parse a command line string into a value of the type the spec demands.
/// Throws Error with a message naming the property on bad input.
PropertyValue parse(const PropertySpec& spec, std::string_view text);

/// Convenience accessors; they throw Error when the map lacks the key or the
/// stored alternative does not match, which only happens on driver bugs.
bool getBool(const PropertyMap& map, std::string_view key);
uint32_t getNumber(const PropertyMap& map, std::string_view key);
std::string getString(const PropertyMap& map, std::string_view key);

} // namespace usbprog
