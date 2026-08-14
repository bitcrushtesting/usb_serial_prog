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

#include "core/registry.h"

#include "core/error.h"
#include "core/text.h"
#include "drivers/ftdi/ftdi_driver.h"

#include <algorithm>

namespace usbprog {

const PropertySpec* Programmer::findProperty(std::string_view name) const {
    const auto& specs = properties();
    const auto it = std::find_if(specs.begin(), specs.end(),
                                 [&](const PropertySpec& spec) { return spec.name == name; });
    return it == specs.end() ? nullptr : &*it;
}

void Registry::add(std::unique_ptr<Driver> driver) {
    if (driver == nullptr) {
        throw Error("internal error: registering a null driver");
    }
    if (byId(driver->id()) != nullptr) {
        throw Error("internal error: duplicate driver id '" + driver->id() + "'");
    }
    drivers_.push_back(std::move(driver));
}

const Driver* Registry::probe(const usb::DeviceInfo& info) const {
    for (const auto& driver : drivers_) {
        if (driver->probe(info)) {
            return driver.get();
        }
    }
    return nullptr;
}

const Driver* Registry::byId(std::string_view id) const {
    for (const auto& driver : drivers_) {
        if (text::iequals(driver->id(), id)) {
            return driver.get();
        }
    }
    return nullptr;
}

const Registry& Registry::builtin() {
    static const Registry kRegistry = [] {
        Registry created;
        registerBuiltinDrivers(created);
        return created;
    }();
    return kRegistry;
}

void registerBuiltinDrivers(Registry& registry) { ftdi::registerDrivers(registry); }

} // namespace usbprog
