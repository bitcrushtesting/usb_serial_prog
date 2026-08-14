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

/// FT232R / FT245R: 128 byte internal EEPROM, five CBUS pins packed two per
/// byte at 0x14-0x16, checksum in the last word.
class Ft232rCodec : public Codec {
public:
    Ft232rCodec();

    static Layout makeLayout();

protected:
    void decodeChip(std::span<const uint8_t> image, PropertyMap& values) const override;
    void encodeChip(const PropertyMap& values, std::vector<uint8_t>& image) const override;
};

} // namespace usbprog::ftdi
