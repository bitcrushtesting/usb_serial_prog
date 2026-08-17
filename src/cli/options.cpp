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

#include "cli/options.h"

#include "core/error.h"
#include "core/text.h"

#include <map>

namespace usbprog::cli {
namespace {

const std::map<std::string, Command, std::less<>>& commands() {
    static const std::map<std::string, Command, std::less<>> kTable = {
        {"help", Command::Help},        {"version", Command::Version},
        {"list", Command::List},        {"drivers", Command::Drivers},
        {"props", Command::Properties}, {"properties", Command::Properties},
        {"info", Command::Info},        {"dump", Command::Dump},
        {"read", Command::Read},        {"write", Command::Write},
        {"set", Command::Set},          {"erase", Command::Erase},
    };
    return kTable;
}

/// Fetch the value of an option, accepting both "--opt value" and "--opt=value".
std::string takeValue(const std::string& name, const std::string& inlineValue, bool hasInline,
                      int& index, int argc, char** argv) {
    if (hasInline) {
        return inlineValue;
    }
    if (index + 1 >= argc) {
        throw Error("option " + name + " needs a value");
    }
    return argv[++index];
}

} // namespace

std::string version() { return std::string("usbprog ") + USBPROG_VERSION; }

std::string usage() {
    return R"(usbprog - read and modify the configuration EEPROM of USB serial chips

Usage:
  usbprog [options] <command> [arguments]

Commands:
  list                  list connected devices and the driver that claims them
  drivers               list the chip families this build supports
  props                 list the settings the selected device understands
  info                  show the current configuration of a device
  dump                  print the EEPROM contents as a hex dump
  read -o FILE          save the raw EEPROM image to FILE
  write -i FILE         write a raw EEPROM image from FILE
  set KEY=VALUE ...     change individual settings
  erase                 erase the EEPROM, restoring the chip's factory defaults

Device selection (all given conditions must match):
  -d, --device VID:PID  match by USB IDs in hex, e.g. 0403:6001
  -s, --serial TEXT     match by serial number
      --bus N           match by USB bus number
      --address N       match by USB device address
      --index N         pick the N-th match, counting from 0
      --driver ID       force a driver instead of probing (see 'usbprog drivers')
      --eeprom-size N   size of an external EEPROM: 128 (93C46) or 256 (93C56,
                        93C66). Only needed when the chip is blank, where the
                        size cannot be measured

Options:
  -o, --output FILE     destination file for 'read'
  -i, --input FILE      source file for 'write'
      --backup FILE     where to save the pre-change backup
      --no-backup       do not save a backup before writing
  -n, --dry-run         show what 'set' or 'write' would do, then stop
  -y, --yes             do not ask for confirmation
      --force           write even when the current EEPROM fails its checksum check
      --all             'list': show devices no driver claims
  -v, --verbose         explain what is going on
  -h, --help            show this help
  -V, --version         show the version

Examples:
  usbprog list
  usbprog info -d 0403:6001
  usbprog read -o backup.bin
  usbprog set product="Widget Bridge" serial_number=WB0001
  usbprog set cbus2=IOMODE cbus3=IOMODE
  usbprog write -i backup.bin
)";
}

Options parse(int argc, char** argv) {
    Options options;
    bool commandSeen = false;
    bool helpRequested = false;
    bool versionRequested = false;

    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument.empty()) {
            continue;
        }

        if (argument.size() > 1 && argument[0] == '-') {
            std::string name = argument;
            std::string inlineValue;
            bool hasInline = false;
            if (const auto equals = argument.find('='); equals != std::string::npos) {
                name = argument.substr(0, equals);
                inlineValue = argument.substr(equals + 1);
                hasInline = true;
            }

            const auto value = [&] {
                return takeValue(name, inlineValue, hasInline, i, argc, argv);
            };

            if (name == "-h" || name == "--help") {
                helpRequested = true;
            } else if (name == "-V" || name == "--version") {
                versionRequested = true;
            } else if (name == "-v" || name == "--verbose") {
                options.verbose = true;
            } else if (name == "-y" || name == "--yes") {
                options.assumeYes = true;
            } else if (name == "-n" || name == "--dry-run") {
                options.dryRun = true;
            } else if (name == "--all") {
                options.showAll = true;
            } else if (name == "--no-backup") {
                options.noBackup = true;
            } else if (name == "--force") {
                options.force = true;
            } else if (name == "-d" || name == "--device") {
                uint16_t vendorId = 0;
                uint16_t productId = 0;
                text::parseVidPid(value(), vendorId, productId);
                options.filter.vendorId = vendorId;
                options.filter.productId = productId;
            } else if (name == "-s" || name == "--serial") {
                options.filter.serial = value();
            } else if (name == "--bus") {
                options.filter.busNumber = static_cast<uint8_t>(text::parseUnsigned(value(), 255));
            } else if (name == "--address") {
                options.filter.deviceAddress =
                    static_cast<uint8_t>(text::parseUnsigned(value(), 255));
            } else if (name == "--index") {
                options.filter.index = text::parseUnsigned(value(), 4095);
            } else if (name == "--driver") {
                options.driverId = value();
            } else if (name == "--eeprom-size") {
                const uint32_t bytes = text::parseUnsigned(value(), 256);
                if (bytes != 128 && bytes != 256) {
                    throw Error("--eeprom-size takes 128 (93C46) or 256 (93C56, 93C66), got " +
                                std::to_string(bytes));
                }
                options.eepromBytes = bytes;
            } else if (name == "-o" || name == "--output") {
                options.outputFile = value();
            } else if (name == "-i" || name == "--input") {
                options.inputFile = value();
            } else if (name == "--backup") {
                options.backupFile = value();
            } else {
                throw Error("unknown option '" + name + "'; run 'usbprog --help'");
            }
            continue;
        }

        if (!commandSeen) {
            const auto it = commands().find(text::toLower(argument));
            if (it == commands().end()) {
                throw Error("unknown command '" + argument + "'; run 'usbprog --help'");
            }
            options.command = it->second;
            commandSeen = true;
        } else {
            options.arguments.push_back(std::move(argument));
        }
    }

    if (versionRequested) {
        options.command = Command::Version;
    } else if (helpRequested || !commandSeen) {
        options.command = Command::Help;
    }
    return options;
}

} // namespace usbprog::cli
