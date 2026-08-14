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

#include "drivers/ftdi/ft232r.h"

#include "core/error.h"

#include <array>

namespace usbprog::ftdi {
namespace {

/// CBUS pin functions, encoded as one nibble per pin.
const std::vector<EnumValue>& cbusFunctions() {
    static const std::vector<EnumValue> kValues = {
        {"TXDEN", 0x00, "transmit enable for RS485"},
        {"PWREN", 0x01, "low after enumeration, high in suspend"},
        {"RXLED", 0x02, "receive activity"},
        {"TXLED", 0x03, "transmit activity"},
        {"TXRXLED", 0x04, "transmit or receive activity"},
        {"SLEEP", 0x05, "low during USB suspend"},
        {"CLK48", 0x06, "48 MHz clock output"},
        {"CLK24", 0x07, "24 MHz clock output"},
        {"CLK12", 0x08, "12 MHz clock output"},
        {"CLK6", 0x09, "6 MHz clock output"},
        {"IOMODE", 0x0A, "CBUS bit bang I/O (CBUS0-CBUS3 only)"},
        {"BITBANG_WR", 0x0B, "synchronous bit bang write strobe"},
        {"BITBANG_RD", 0x0C, "synchronous bit bang read strobe"},
    };
    return kValues;
}

struct CbusPin {
    const char* name;
    uint16_t address;
    bool highNibble;
};

/// CBUS0 and CBUS1 share 0x14, CBUS2 and CBUS3 share 0x15, CBUS4 sits in the
/// low nibble of 0x16.
constexpr std::array<CbusPin, 5> kCbusPins = {{
    {"cbus0", 0x14, false},
    {"cbus1", 0x14, true},
    {"cbus2", 0x15, false},
    {"cbus3", 0x15, true},
    {"cbus4", 0x16, false},
}};

constexpr uint16_t kDriveAddress = 0x00;
constexpr uint8_t kHighCurrentDrive = 0x04;
constexpr uint8_t kExternalOscillator = 0x02;

} // namespace

Codec::Layout Ft232rCodec::makeLayout() {
    Layout layout;
    layout.size = 128;
    layout.stringAreaBegin = 0x18;
    layout.stringAreaEnd = 0x7E; // the last word holds the checksum
    layout.checksumWord = 0x3F;
    layout.checksumWords = {WordRange{0x00, 0x3F}};
    return layout;
}

Ft232rCodec::Ft232rCodec() : Codec(makeLayout()) {
    PropertySpec highCurrent;
    highCurrent.name = "high_current_io";
    highCurrent.type = PropertyType::Bool;
    highCurrent.help = "raise the I/O drive strength (high current I/O mode)";
    addProperty(std::move(highCurrent));

    PropertySpec oscillator;
    oscillator.name = "external_oscillator";
    oscillator.type = PropertyType::Bool;
    oscillator.help = "use an external oscillator instead of the internal one";
    addProperty(std::move(oscillator));

    for (const CbusPin& pin : kCbusPins) {
        addProperty(cbusProperty(pin.name, cbusFunctions(),
                                 std::string("function of the ") + pin.name + " pin"));
    }
}

void Ft232rCodec::decodeChip(std::span<const uint8_t> image, PropertyMap& values) const {
    const uint8_t driveByte = image[kDriveAddress];
    values["high_current_io"] = (driveByte & kHighCurrentDrive) != 0;
    values["external_oscillator"] = (driveByte & kExternalOscillator) != 0;

    for (const CbusPin& pin : kCbusPins) {
        values[pin.name] = nibble(image[pin.address], pin.highNibble);
    }
}

void Ft232rCodec::encodeChip(const PropertyMap& values, std::vector<uint8_t>& image) const {
    uint8_t driveByte = image[kDriveAddress];
    driveByte =
        static_cast<uint8_t>(getBool(values, "high_current_io") ? (driveByte | kHighCurrentDrive)
                                                                : (driveByte & ~kHighCurrentDrive));
    driveByte = static_cast<uint8_t>(getBool(values, "external_oscillator")
                                         ? (driveByte | kExternalOscillator)
                                         : (driveByte & ~kExternalOscillator));
    image[kDriveAddress] = driveByte;

    for (const CbusPin& pin : kCbusPins) {
        const uint32_t function = getNumber(values, pin.name);
        if (function > 0x0F) {
            throw Error(std::string(pin.name) + ": the FT232R stores CBUS functions in a nibble, "
                                                "so values above 0x0f are not representable");
        }
        uint8_t byte = image[pin.address];
        setNibble(byte, pin.highNibble, function);
        image[pin.address] = byte;
    }
}

} // namespace usbprog::ftdi
