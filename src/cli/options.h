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

#include "core/usb.h"

#include <string>
#include <vector>

namespace usbprog::cli {

enum class Command {
    Help,
    Version,
    List,
    Drivers,
    Properties,
    Info,
    Dump,
    Read,
    Write,
    Set,
    Erase,
};

struct Options {
    Command command = Command::Help;
    std::vector<std::string> arguments; ///< key=value pairs for `set`

    usb::DeviceFilter filter;
    std::string driverId; ///< force a driver instead of probing

    std::string inputFile;
    std::string outputFile;
    std::string backupFile;

    bool assumeYes = false;
    bool verbose = false;
    bool dryRun = false;
    bool showAll = false; ///< `list`: include devices without a driver
    bool noBackup = false;
    bool force = false; ///< ignore the checksum safety check
};

/// Parse the command line. Throws Error on anything malformed.
Options parse(int argc, char** argv);

std::string usage();
std::string version();

} // namespace usbprog::cli
