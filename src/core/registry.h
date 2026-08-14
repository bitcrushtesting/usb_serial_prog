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

#include "core/driver.h"

#include <memory>
#include <string_view>
#include <vector>

namespace usbprog {

/// The set of drivers this binary knows about.
class Registry {
public:
    void add(std::unique_ptr<Driver> driver);

    const std::vector<std::unique_ptr<Driver>>& drivers() const { return drivers_; }

    /// First driver that recognises the device, or nullptr.
    const Driver* probe(const usb::DeviceInfo& info) const;

    const Driver* byId(std::string_view id) const;

    /// Registry populated with the built-in drivers. Registration is explicit
    /// (see registerBuiltinDrivers) rather than relying on static initialisers,
    /// which the linker likes to drop from static libraries.
    static const Registry& builtin();

private:
    std::vector<std::unique_ptr<Driver>> drivers_;
};

/// Add every driver compiled into this binary. New chip families are added
/// here; nothing else in the CLI needs to change.
void registerBuiltinDrivers(Registry& registry);

} // namespace usbprog
