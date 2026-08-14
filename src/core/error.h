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

#include <stdexcept>
#include <string>

namespace usbprog {

/// Base class for every error this tool raises deliberately.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A libusb call failed. Carries the raw libusb error code.
class UsbError : public Error {
public:
    UsbError(const std::string& what, int code) : Error(what), code_(code) {}
    int code() const noexcept { return code_; }

private:
    int code_;
};

} // namespace usbprog
