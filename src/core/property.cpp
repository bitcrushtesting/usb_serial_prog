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

#include "core/property.h"

#include "core/error.h"
#include "core/text.h"

#include <algorithm>
#include <optional>

namespace usbprog {
namespace {

const PropertyValue& lookup(const PropertyMap& map, std::string_view key) {
    const auto it = map.find(key);
    if (it == map.end()) {
        throw Error("internal error: property '" + std::string(key) + "' is not set");
    }
    return it->second;
}

} // namespace

const EnumValue* PropertySpec::find(std::string_view wanted) const {
    const auto it = std::find_if(values.begin(), values.end(),
                                 [&](const EnumValue& v) { return text::iequals(v.name, wanted); });
    return it == values.end() ? nullptr : &*it;
}

const EnumValue* PropertySpec::find(uint32_t wanted) const {
    const auto it = std::find_if(values.begin(), values.end(),
                                 [&](const EnumValue& v) { return v.value == wanted; });
    return it == values.end() ? nullptr : &*it;
}

std::string format(const PropertySpec& spec, const PropertyValue& value) {
    switch (spec.type) {
    case PropertyType::Bool:
        return std::get<bool>(value) ? "on" : "off";
    case PropertyType::Number: {
        const uint32_t n = std::get<uint32_t>(value);
        std::string out =
            spec.hexadecimal ? "0x" + text::hex16(static_cast<uint16_t>(n)) : std::to_string(n);
        if (!spec.unit.empty()) {
            out += " " + spec.unit;
        }
        return out;
    }
    case PropertyType::String: {
        const auto& s = std::get<std::string>(value);
        return s.empty() ? "(empty)" : s;
    }
    case PropertyType::Enum: {
        const uint32_t n = std::get<uint32_t>(value);
        if (const EnumValue* v = spec.find(n)) {
            return v->name;
        }
        return "unknown(0x" + text::hex8(static_cast<uint8_t>(n)) + ")";
    }
    }
    return {};
}

PropertyValue parse(const PropertySpec& spec, std::string_view text) {
    const std::string value = text::trim(text);
    switch (spec.type) {
    case PropertyType::Bool: {
        static constexpr std::string_view kTrue[] = {"on", "1", "true", "yes", "y", "enable"};
        static constexpr std::string_view kFalse[] = {"off", "0", "false", "no", "n", "disable"};
        for (auto candidate : kTrue) {
            if (text::iequals(value, candidate)) {
                return true;
            }
        }
        for (auto candidate : kFalse) {
            if (text::iequals(value, candidate)) {
                return false;
            }
        }
        throw Error(spec.name + ": expected on/off, got '" + value + "'");
    }
    case PropertyType::Number: {
        uint32_t n = 0;
        try {
            n = text::parseUnsigned(value, spec.maximum);
        } catch (const Error& e) {
            throw Error(spec.name + ": " + e.what());
        }
        if (n < spec.minimum) {
            throw Error(spec.name + ": value must be at least " + std::to_string(spec.minimum));
        }
        if (spec.step > 1 && (n % spec.step) != 0) {
            throw Error(spec.name + ": value must be a multiple of " + std::to_string(spec.step));
        }
        return n;
    }
    case PropertyType::String: {
        std::string s(text); // keep leading/trailing spaces the user asked for
        if (spec.maxChars != 0 && text::utf16Length(s) > spec.maxChars) {
            throw Error(spec.name + ": at most " + std::to_string(spec.maxChars) +
                        " characters are allowed");
        }
        return s;
    }
    case PropertyType::Enum: {
        if (const EnumValue* v = spec.find(value)) {
            return v->value;
        }
        // Also accept the raw numeric encoding, for values a driver does not name yet.
        if (const std::optional<uint32_t> number = text::tryParseUnsigned(value, 0xFF)) {
            return *number;
        }
        std::string names;
        for (const auto& v : spec.values) {
            names += (names.empty() ? "" : ", ") + v.name;
        }
        throw Error(spec.name + ": '" + value + "' is not a valid value; expected one of " + names);
    }
    }
    throw Error("internal error: unhandled property type");
}

bool getBool(const PropertyMap& map, std::string_view key) {
    const auto* v = std::get_if<bool>(&lookup(map, key));
    if (!v) {
        throw Error("internal error: property '" + std::string(key) + "' is not a boolean");
    }
    return *v;
}

uint32_t getNumber(const PropertyMap& map, std::string_view key) {
    const auto* v = std::get_if<uint32_t>(&lookup(map, key));
    if (!v) {
        throw Error("internal error: property '" + std::string(key) + "' is not a number");
    }
    return *v;
}

std::string getString(const PropertyMap& map, std::string_view key) {
    const auto* v = std::get_if<std::string>(&lookup(map, key));
    if (!v) {
        throw Error("internal error: property '" + std::string(key) + "' is not a string");
    }
    return *v;
}

} // namespace usbprog
