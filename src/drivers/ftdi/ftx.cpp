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

#include "drivers/ftdi/ftx.h"

#include <array>

namespace usbprog::ftdi {
namespace {

/// FT-X CBUS functions. Unlike the FT232R these take a whole byte per pin and
/// use a different numbering.
const std::vector<EnumValue>& cbusFunctions() {
    static const std::vector<EnumValue> kValues = {
        {"TRISTATE", 0x00, "pin is not driven"},
        {"RXLED", 0x01, "receive activity"},
        {"TXLED", 0x02, "transmit activity"},
        {"TXRXLED", 0x03, "transmit or receive activity"},
        {"PWREN", 0x04, "low after enumeration, high in suspend"},
        {"SLEEP", 0x05, "low during USB suspend"},
        {"DRIVE_0", 0x06, "drive the pin low"},
        {"DRIVE_1", 0x07, "drive the pin high"},
        {"IOMODE", 0x08, "CBUS bit bang I/O"},
        {"TXDEN", 0x09, "transmit enable for RS485"},
        {"CLK24", 0x0A, "24 MHz clock output"},
        {"CLK12", 0x0B, "12 MHz clock output"},
        {"CLK6", 0x0C, "6 MHz clock output"},
        {"BAT_DETECT", 0x0D, "battery charger detected"},
        {"BAT_DETECT_NEG", 0x0E, "battery charger detected, inverted"},
        {"I2C_TXE", 0x0F, "I2C transmit buffer empty"},
        {"I2C_RXF", 0x10, "I2C receive buffer full"},
        {"VBUS_SENSE", 0x11, "VBUS sense input"},
        {"BITBANG_WR", 0x12, "synchronous bit bang write strobe"},
        {"BITBANG_RD", 0x13, "synchronous bit bang read strobe"},
        {"TIME_STAMP", 0x14, "toggles on every USB SOF"},
        {"AWAKE", 0x15, "low while the device is not suspended"},
    };
    return kValues;
}

constexpr uint16_t kCbusBase = 0x1A;
constexpr std::array<const char*, 4> kCbusNames = {"cbus0", "cbus1", "cbus2", "cbus3"};

} // namespace

Codec::Layout FtxCodec::makeLayout() {
    Layout layout;
    layout.size = 256;
    layout.stringAreaBegin = 0xA0;
    layout.stringAreaEnd = 0xFE; // the last word holds the checksum
    layout.checksumWord = 0x7F;
    // The FT-X checksum covers the header and the upper half of the EEPROM; the
    // words in between hold the user area and are excluded.
    layout.checksumWords = {WordRange{0x00, 0x12}, WordRange{0x40, 0x7F}};
    return layout;
}

FtxCodec::FtxCodec() : Codec(makeLayout()) {
    for (const char* name : kCbusNames) {
        addProperty(
            cbusProperty(name, cbusFunctions(), std::string("function of the ") + name + " pin"));
    }
}

void FtxCodec::decodeChip(std::span<const uint8_t> image, PropertyMap& values) const {
    for (std::size_t i = 0; i < kCbusNames.size(); ++i) {
        values[kCbusNames[i]] = static_cast<uint32_t>(image[kCbusBase + i]);
    }
}

void FtxCodec::encodeChip(const PropertyMap& values, std::vector<uint8_t>& image) const {
    for (std::size_t i = 0; i < kCbusNames.size(); ++i) {
        image[kCbusBase + i] = static_cast<uint8_t>(getNumber(values, kCbusNames[i]));
    }
}

} // namespace usbprog::ftdi
