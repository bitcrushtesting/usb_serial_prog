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

#include "cli/commands.h"
#include "cli/options.h"
#include "core/error.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        const usbprog::cli::Options options = usbprog::cli::parse(argc, argv);
        return usbprog::cli::run(options);
    } catch (const usbprog::Error& error) {
        std::cerr << "usbprog: " << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "usbprog: unexpected error: " << error.what() << "\n";
        return 3;
    }
}
