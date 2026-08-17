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

#include "drivers/ftdi/ft4232h.h"

#include "core/error.h"

#include <array>
#include <cstddef>
#include <string>

namespace usbprog::ftdi {
namespace {

/// Everything that differs between the four channels. The FT4232H interleaves
/// them: the VCP bits for A and B sit in the low nibble of 0x00 and 0x01 and
/// those for C and D in the high nibble of the same two bytes, while the pad
/// settings pair A with B in 0x0C and C with D in 0x0D.
struct Channel {
    const char* suffix;  ///< property suffix, "a" to "d"
    const char* label;   ///< how the datasheet names the channel
    uint16_t vcpAddress; ///< 0x00 for A and C, 0x01 for B and D
    uint8_t vcpMask;     ///< 0x08 in the low nibble, 0x80 in the high one
    uint8_t rs485Mask;   ///< one bit each in 0x0B
    uint16_t padAddress; ///< 0x0C for A and B, 0x0D for C and D
    bool padHighNibble;  ///< B and D take the high nibble of that byte
};

constexpr std::array<Channel, 4> kChannels = {{
    {"a", "A", 0x00, 0x08, 0x10, 0x0C, false},
    {"b", "B", 0x01, 0x08, 0x20, 0x0C, true},
    {"c", "C", 0x00, 0x80, 0x40, 0x0D, false},
    {"d", "D", 0x01, 0x80, 0x80, 0x0D, true},
}};

/// Bits inside a pad settings nibble.
constexpr uint8_t kDriveMask = 0x03; ///< 0-3, meaning 4, 8, 12 and 16 mA
constexpr uint8_t kSlowSlew = 0x04;
constexpr uint8_t kSchmitt = 0x08;

std::string name(const char* stem, const Channel& channel) {
    return std::string(stem) + "_" + channel.suffix;
}

PropertySpec boolProperty(const std::string& propertyName, const std::string& help) {
    PropertySpec spec;
    spec.name = propertyName;
    spec.type = PropertyType::Bool;
    spec.help = help;
    return spec;
}

} // namespace

Codec::Layout Ft4232hCodec::makeLayout(std::size_t eepromBytes) {
    if (eepromBytes != 128 && eepromBytes != 256) {
        throw Error("the FT4232H takes a 93C46 (128 bytes) or a 93C56/93C66 (256 bytes), not " +
                    std::to_string(eepromBytes) + " bytes");
    }

    Layout layout;
    layout.size = eepromBytes;
    // The chip wraps word addresses modulo the part fitted, so on a 93C46 the
    // string area at 0x9A lands at 0x1A. writeStrings() sets bit 7 of the
    // offset byte and readString() masks it with the size, so the byte on the
    // chip reads 0x9A either way and both parts round-trip.
    layout.stringAreaBegin = eepromBytes == 128 ? 0x1A : 0x9A;
    layout.stringAreaEnd = static_cast<uint16_t>(eepromBytes - 2); // last word holds the checksum
    layout.checksumWord = static_cast<uint16_t>((eepromBytes / 2) - 1);
    // Unlike the FT-X the H series checksums one uninterrupted run of words.
    layout.checksumWords = {WordRange{0x00, layout.checksumWord}};
    // 0x0B is the RS485 enable byte on this chip, not the inverter byte.
    layout.signalInversion = false;
    return layout;
}

Ft4232hCodec::Ft4232hCodec(std::size_t eepromBytes) : Codec(makeLayout(eepromBytes)) {
    for (const Channel& channel : kChannels) {
        const std::string label = channel.label;
        addProperty(
            boolProperty(name("vcp", channel),
                         "expose channel " + label + " as a virtual COM port (off: D2XX only)"));
        addProperty(boolProperty(name("rs485", channel),
                                 "drive the TXDEN handshake on channel " + label + " for RS485"));

        PropertySpec drive;
        drive.name = name("drive", channel);
        drive.type = PropertyType::Number;
        drive.unit = "mA";
        drive.minimum = 4;
        drive.maximum = 16;
        drive.step = 4;
        drive.help = "pad drive strength of channel " + label + " (4, 8, 12 or 16)";
        addProperty(std::move(drive));

        addProperty(boolProperty(name("schmitt", channel),
                                 "use Schmitt trigger inputs on channel " + label));
        addProperty(boolProperty(name("slow_slew", channel),
                                 "limit the output slew rate on channel " + label));
    }
}

void Ft4232hCodec::decodeChip(std::span<const uint8_t> image, PropertyMap& values) const {
    const uint8_t rs485Byte = image[addr::kInvert];

    for (const Channel& channel : kChannels) {
        values[name("vcp", channel)] = (image[channel.vcpAddress] & channel.vcpMask) != 0;
        values[name("rs485", channel)] = (rs485Byte & channel.rs485Mask) != 0;

        const auto pad =
            static_cast<uint8_t>(nibble(image[channel.padAddress], channel.padHighNibble));
        // Stored as 0-3; the datasheet lists those as 4, 8, 12 and 16 mA.
        values[name("drive", channel)] = static_cast<uint32_t>(((pad & kDriveMask) + 1) * 4);
        values[name("schmitt", channel)] = (pad & kSchmitt) != 0;
        values[name("slow_slew", channel)] = (pad & kSlowSlew) != 0;
    }
}

void Ft4232hCodec::encodeChip(const PropertyMap& values, std::vector<uint8_t>& image) const {
    // The low nibble of the RS485 byte is not modelled, so it is preserved
    // rather than rebuilt from scratch.
    uint8_t rs485Byte = image[addr::kInvert];

    for (const Channel& channel : kChannels) {
        uint8_t vcpByte = image[channel.vcpAddress];
        vcpByte = static_cast<uint8_t>(getBool(values, name("vcp", channel))
                                           ? (vcpByte | channel.vcpMask)
                                           : (vcpByte & ~channel.vcpMask));
        image[channel.vcpAddress] = vcpByte;

        rs485Byte = static_cast<uint8_t>(getBool(values, name("rs485", channel))
                                             ? (rs485Byte | channel.rs485Mask)
                                             : (rs485Byte & ~channel.rs485Mask));

        const uint32_t milliamps = getNumber(values, name("drive", channel));
        if (milliamps < 4 || milliamps > 16 || (milliamps % 4) != 0) {
            throw Error(name("drive", channel) + ": the FT4232H supports 4, 8, 12 and 16 mA, got " +
                        std::to_string(milliamps));
        }

        // Every bit of the nibble is modelled, so replacing it whole loses
        // nothing.
        auto pad = static_cast<uint8_t>((milliamps / 4) - 1);
        if (getBool(values, name("schmitt", channel))) {
            pad |= kSchmitt;
        }
        if (getBool(values, name("slow_slew", channel))) {
            pad |= kSlowSlew;
        }
        uint8_t padByte = image[channel.padAddress];
        setNibble(padByte, channel.padHighNibble, pad);
        image[channel.padAddress] = padByte;
    }

    image[addr::kInvert] = rs485Byte;
}

} // namespace usbprog::ftdi
