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

#include "core/property.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace usbprog::ch34x {

/// Byte offsets in the CH340B configuration data area.
///
/// Source of truth is the WCH datasheet "CH340 Datasheet (I)", version 3B,
/// section 5.2 "Configuration information of CH340B". Unlike the FTDI parts
/// there is no checksum and no free-form string heap: every field sits at a
/// fixed address and everything outside them is reserved.
namespace addr {
constexpr uint16_t kSig = 0x00;          ///< configuration valid indicator
constexpr uint16_t kMode = 0x01;         ///< serial mode
constexpr uint16_t kConfig = 0x02;       ///< chip configuration bits
constexpr uint16_t kWriteProtect = 0x03; ///< 0x57 makes the area read only
constexpr uint16_t kVendorId = 0x04;     ///< 2 bytes, little endian
constexpr uint16_t kProductId = 0x06;    ///< 2 bytes, little endian
constexpr uint16_t kMaxPower = 0x0A;     ///< in 2 mA units
constexpr uint16_t kSerialNumber = 0x10; ///< 8 ASCII characters, not a descriptor
constexpr uint16_t kProduct = 0x1A;      ///< USB string descriptor, to the end of the area
} // namespace addr

/// The whole configuration area, and the constants that give its bytes meaning.
namespace config {
constexpr std::size_t kBytes = 0x40;

/// SIG values. The chip ignores the whole area unless SIG matches the part, in
/// which case it falls back to the vendor defaults, so SIG doubles as the
/// erase mechanism: clear it and the chip is factory fresh again.
constexpr uint8_t kSigInternal = 0x58; ///< CH340B, configuration held on die
constexpr uint8_t kSigExternal = 0x53; ///< CH340H/S, configuration in an external part

constexpr uint8_t kModeSerial = 0x23; ///< the only mode the datasheet defines

/// WP. Any other value leaves the area writable; this one is a one way door,
/// which is why nothing in this driver ever writes it.
constexpr uint8_t kWriteProtected = 0x57;

/// CFG bit 5: set means the serial number is *not* reported.
constexpr uint8_t kSerialDisabled = 0x20;

constexpr std::size_t kSerialChars = 8;

/// The product string is stored as a USB string descriptor: a length byte, the
/// descriptor type 0x03, then UTF-16LE. The datasheet caps the length byte
/// below 0x26, and a descriptor is always an even number of bytes.
constexpr std::size_t kProductDescriptorBytes = 0x24;
constexpr std::size_t kProductChars = (kProductDescriptorBytes - 2) / 2;

/// Datasheet defaults, used when the area is not active. They are what the
/// chip behaves as in that state, so reporting them is the truthful decode of
/// a factory part rather than an invention.
constexpr uint16_t kDefaultVendorId = 0x1A86;
constexpr uint16_t kDefaultProductId = 0x7523;
constexpr uint8_t kDefaultConfig = 0xFE;
constexpr uint8_t kDefaultMaxPowerUnits = 0x31;
} // namespace config

/// Translates between a CH340B configuration image and named properties.
///
/// Two things set this apart from the FTDI codecs and are worth knowing before
/// reading the implementation:
///
///   * There is no checksum. Validity is a signature byte instead, and a chip
///     whose signature does not match runs on its vendor defaults.
///   * Because of that, decoding an inactive area reports those defaults
///     rather than the raw (usually zeroed) bytes, and encoding always sets
///     the signature. Otherwise `set serial_number=...` on a factory part
///     would commit a 0 mA power descriptor and a 0000:0000 identity picked up
///     from bytes the chip was never reading in the first place.
class Ch340Config {
public:
    Ch340Config();

    static std::size_t size() { return config::kBytes; }
    const std::vector<PropertySpec>& properties() const { return properties_; }

    PropertyMap decode(std::span<const uint8_t> image) const;
    void encode(const PropertyMap& values, std::vector<uint8_t>& image) const;

    /// True when the area is one this driver understands: either inactive, in
    /// which case the chip uses its vendor defaults, or active and carrying
    /// the serial mode the datasheet defines. There is no checksum to verify;
    /// the name comes from the Programmer interface.
    bool verifyChecksum(std::span<const uint8_t> image) const;

    /// Whether the chip is actually reading this area, i.e. SIG says so.
    static bool isActive(std::span<const uint8_t> image);

    /// Whether the area has been locked against further writes.
    static bool isWriteProtected(std::span<const uint8_t> image);

private:
    void addProperty(PropertySpec spec);
    static void requireSize(std::size_t size);

    std::vector<PropertySpec> properties_;
};

} // namespace usbprog::ch34x
