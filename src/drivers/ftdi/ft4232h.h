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

#include "drivers/ftdi/ftdi_codec.h"

namespace usbprog::ftdi {

/// FT4232H: four independent UARTs, 256 byte EEPROM, strings at 0x9A.
///
/// Unlike the FT232R and FT-X this chip has no CBUS pins and no signal
/// inverters. What it does have is four of everything else, so the properties
/// come in per-channel sets suffixed _a to _d:
///
///   - vcp_*       expose the channel as a virtual COM port (otherwise D2XX)
///   - rs485_*     drive the TXDEN handshake for RS485 transceivers
///   - drive_*     pad drive strength, 4/8/12/16 mA
///   - schmitt_*   Schmitt trigger inputs
///   - slow_slew_* slew rate limiting
///
/// Two properties of the layout differ from the other families and are the
/// reason this class exists rather than a plain Layout:
///
///   - 0x0B holds the RS485 enables, where the other chips keep the signal
///     inverters, so Layout::signalInversion is off and this codec owns the
///     byte (see encodeChip()).
///   - the VCP bits live in the *high* nibble of 0x00 and 0x01 for channels C
///     and D, and in the low nibble for A and B.
///
/// The EEPROM is external, and which part is fitted changes the layout. FTDI
/// chips wrap EEPROM word addresses modulo the size of the part, so on a 93C46
/// (64 words) the string area at 0x9A folds down onto 0x1A. That is a supported
/// arrangement rather than a broken one: the offset byte still records 0x9A and
/// readers mask it with the size, so both parts round-trip. What it is not is
/// optional — writing a 256 byte image to a 93C46 wraps the upper half onto the
/// header and destroys it, so the size has to be known before writing.
///
///   93C46          128 bytes, 64 words, strings at 0x1A, checksum word 0x3F
///   93C56, 93C66   256 bytes, strings at 0x9A, checksum word 0x7F
///
/// A blank EEPROM mirrors identically at every size and so cannot be measured;
/// see FtdiDriver::attach, which asks the user in that case.
///
/// This layout follows the public description of the chip and has had far less
/// exposure to real silicon than the FT232R path.
class Ft4232hCodec : public Codec {
public:
    /// \param eepromBytes 128 for a 93C46, 256 for a 93C56 or 93C66.
    explicit Ft4232hCodec(std::size_t eepromBytes);

    static Layout makeLayout(std::size_t eepromBytes);

protected:
    void decodeChip(std::span<const uint8_t> image, PropertyMap& values) const override;
    void encodeChip(const PropertyMap& values, std::vector<uint8_t>& image) const override;
};

} // namespace usbprog::ftdi
