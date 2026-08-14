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

/// FT-X family (FT230X, FT231X, FT234XD): 256 byte EEPROM, four CBUS pins with
/// a full byte each at 0x1A-0x1D, strings at 0xA0, and a checksum that skips
/// the middle of the memory.
///
/// Support here is less exercised than the FT232R path. The CLI refuses to
/// write when it cannot reproduce the checksum of the image already on the
/// chip, which is the practical guard against a wrong layout assumption.
class FtxCodec : public Codec {
public:
    FtxCodec();

    static Layout makeLayout();

protected:
    void decodeChip(std::span<const uint8_t> image, PropertyMap& values) const override;
    void encodeChip(const PropertyMap& values, std::vector<uint8_t>& image) const override;
};

} // namespace usbprog::ftdi
