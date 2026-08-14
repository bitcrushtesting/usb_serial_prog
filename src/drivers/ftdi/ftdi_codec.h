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

namespace usbprog::ftdi {

/// EEPROM byte offsets shared by all FTDI chips that use the "new" layout
/// (FT232R and the FT-X family both do).
namespace addr {
constexpr uint16_t kVendorId = 0x02;         ///< 2 bytes, little endian
constexpr uint16_t kProductId = 0x04;        ///< 2 bytes, little endian
constexpr uint16_t kRelease = 0x06;          ///< 2 bytes, chip generation
constexpr uint16_t kConfigDescriptor = 0x08; ///< bus power flags
constexpr uint16_t kMaxPower = 0x09;         ///< in 2 mA units
constexpr uint16_t kChipConfig = 0x0A;
constexpr uint16_t kInvert = 0x0B; ///< per-signal inversion
constexpr uint16_t kManufacturerOffset = 0x0E;
constexpr uint16_t kManufacturerLength = 0x0F;
constexpr uint16_t kProductOffset = 0x10;
constexpr uint16_t kProductLength = 0x11;
constexpr uint16_t kSerialOffset = 0x12;
constexpr uint16_t kSerialLength = 0x13;
} // namespace addr

/// Bits in the configuration descriptor byte (0x08).
namespace config {
constexpr uint8_t kReserved = 0x80; ///< always set, mirrors bmAttributes
constexpr uint8_t kSelfPowered = 0x40;
constexpr uint8_t kRemoteWakeup = 0x20;
} // namespace config

/// Bits in the chip configuration byte (0x0A).
namespace chip {
constexpr uint8_t kInIsochronous = 0x01;
constexpr uint8_t kOutIsochronous = 0x02;
constexpr uint8_t kSuspendPullDowns = 0x04;
constexpr uint8_t kUseSerial = 0x08;
} // namespace chip

/// Bits in the inversion byte (0x0B).
namespace invert {
constexpr uint8_t kTxd = 0x01;
constexpr uint8_t kRxd = 0x02;
constexpr uint8_t kRts = 0x04;
constexpr uint8_t kCts = 0x08;
constexpr uint8_t kDtr = 0x10;
constexpr uint8_t kDsr = 0x20;
constexpr uint8_t kDcd = 0x40;
constexpr uint8_t kRi = 0x80;
} // namespace invert

/// Half-open range of 16 bit words, used to describe the checksummed area.
struct WordRange {
    uint16_t begin = 0;
    uint16_t end = 0;
};

/// Translates between an FTDI EEPROM image and named properties.
///
/// Encoding is a read-modify-write on the existing image: only the bytes a
/// property owns are touched, so factory data and fields this tool does not
/// model survive a `set`.
class Codec {
public:
    struct Layout {
        std::size_t size = 128;          ///< EEPROM size in bytes
        uint16_t stringAreaBegin = 0x18; ///< first byte usable for strings
        uint16_t stringAreaEnd = 0x7E;   ///< one past the last usable byte
        uint16_t checksumWord = 0x3F;    ///< word index holding the checksum
        std::vector<WordRange> checksumWords{{0x00, 0x3F}};
    };

    explicit Codec(Layout layout);
    virtual ~Codec() = default;

    // Codecs are created by their driver and used through this base class.
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    Codec(Codec&&) = delete;
    Codec& operator=(Codec&&) = delete;

    const Layout& layout() const { return layout_; }
    const std::vector<PropertySpec>& properties() const { return properties_; }

    PropertyMap decode(std::span<const uint8_t> image) const;
    void encode(const PropertyMap& values, std::vector<uint8_t>& image) const;

    /// FTDI's checksum: seed 0xAAAA, then per word XOR followed by a rotate
    /// left by one. Which words take part depends on the chip.
    uint16_t computeChecksum(std::span<const uint8_t> image) const;
    uint16_t storedChecksum(std::span<const uint8_t> image) const;
    bool verifyChecksum(std::span<const uint8_t> image) const;

    /// How many UTF-16 code units all three strings may use together.
    std::size_t stringBudget() const;

protected:
    void addProperty(PropertySpec spec);

    /// Hooks for chip specific fields (CBUS pins, drive strength, ...).
    virtual void decodeChip(std::span<const uint8_t> image, PropertyMap& values) const {
        (void)image;
        (void)values;
    }
    virtual void encodeChip(const PropertyMap& values, std::vector<uint8_t>& image) const {
        (void)values;
        (void)image;
    }

    /// Helpers for derived codecs.
    static uint32_t nibble(uint8_t byte, bool high);
    static void setNibble(uint8_t& byte, bool high, uint32_t value);
    static PropertySpec cbusProperty(const std::string& name, std::vector<EnumValue> values,
                                     const std::string& help);

private:
    void addCommonProperties();
    std::string readString(std::span<const uint8_t> image, uint16_t offsetAddr,
                           uint16_t lengthAddr) const;
    void writeStrings(std::vector<uint8_t>& image, const std::string& manufacturer,
                      const std::string& product, const std::string& serial) const;
    void requireSize(std::size_t size) const;

    Layout layout_;
    std::vector<PropertySpec> properties_;
};

} // namespace usbprog::ftdi
