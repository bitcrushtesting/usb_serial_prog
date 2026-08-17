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

#include "cli/output.h"
#include "core/error.h"
#include "core/registry.h"
#include "core/text.h"

#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace usbprog::cli {
namespace {

struct Target {
    usb::Device device;
    usb::DeviceInfo info;
    const Driver* driver = nullptr;
};

std::string describe(const usb::DeviceInfo& info, const Driver* driver) {
    std::string line = padRight(info.address(), 8) + "  " + text::hex16(info.vendorId) + ":" +
                       text::hex16(info.productId);
    line += "  " + padRight(driver != nullptr ? driver->id() : "-", 8);
    // Plenty of devices pad their string descriptors; that is their business,
    // but it should not ruin the columns here.
    std::string description = text::trim(info.product);
    const std::string manufacturer = text::trim(info.manufacturer);
    if (!manufacturer.empty()) {
        description = manufacturer + (description.empty() ? "" : " " + description);
    }
    if (description.empty()) {
        description = info.descriptorsReadable ? "(no strings)" : "(not readable)";
    }
    line += "  " + description;
    if (!info.serial.empty()) {
        line += "  [" + info.serial + "]";
    }
    return line;
}

/// All devices that satisfy the filter and have a driver. With --driver the
/// probe is skipped, so devices with a reprogrammed vendor ID stay reachable.
std::vector<Target> findTargets(const usb::Context& context, const Registry& registry,
                                const Options& options) {
    const Driver* forced = nullptr;
    if (!options.driverId.empty()) {
        forced = registry.byId(options.driverId);
        if (forced == nullptr) {
            throw Error("unknown driver '" + options.driverId +
                        "'; run 'usbprog drivers' for the list");
        }
    }

    std::vector<Target> targets;
    for (const usb::Device& device : context.devices()) {
        usb::DeviceInfo info;
        try {
            info = device.info();
        } catch (const Error&) {
            continue; // a device that disappeared or refuses to talk to us
        }
        if (!options.filter.matches(info)) {
            continue;
        }
        const Driver* driver = forced != nullptr ? forced : registry.probe(info);
        if (driver == nullptr) {
            continue;
        }
        targets.push_back(Target{device, std::move(info), driver});
    }
    return targets;
}

/// Exactly one device, or a message explaining what to do about it.
Target selectTarget(const usb::Context& context, const Registry& registry, const Options& options) {
    std::vector<Target> targets = findTargets(context, registry, options);

    if (options.filter.index) {
        const std::size_t index = *options.filter.index;
        if (index >= targets.size()) {
            throw Error("--index " + std::to_string(index) + " is out of range: " +
                        std::to_string(targets.size()) + " matching device(s) found");
        }
        return std::move(targets[index]);
    }

    const std::string what =
        options.filter.empty() ? "no supported device found"
                               : "no supported device found matching " + options.filter.describe();

    if (targets.empty()) {
        throw Error(what +
                    ".\nRun 'usbprog list --all' to see what is connected. If the chip has a "
                    "reprogrammed vendor ID, select it with --device and --driver.");
    }
    if (targets.size() > 1) {
        std::string message = "several devices match; narrow it down with --device, --serial, "
                              "--bus/--address or --index:\n";
        for (std::size_t i = 0; i < targets.size(); ++i) {
            message += "  --index " + std::to_string(i) + "  " +
                       describe(targets[i].info, targets[i].driver) + "\n";
        }
        throw Error(message);
    }
    return std::move(targets.front());
}

struct Attachment {
    Target target;
    std::unique_ptr<Programmer> programmer;
};

Attachment attach(const usb::Context& context, const Registry& registry, const Options& options) {
    Target target = selectTarget(context, registry, options);
    usb::Handle handle = target.device.open();
    AttachOptions attachOptions;
    attachOptions.eepromBytes = options.eepromBytes;
    auto programmer = target.driver->attach(std::move(handle), target.info, attachOptions);
    for (const std::string& warning : programmer->warnings()) {
        std::cerr << "warning: " << warning << "\n";
    }
    return Attachment{std::move(target), std::move(programmer)};
}

void printDeviceHeader(const Target& target, const Programmer& programmer) {
    std::cout << "Device    " << target.info.address() << " (" << text::hex16(target.info.vendorId)
              << ":" << text::hex16(target.info.productId) << ")\n";
    std::cout << "Chip      " << programmer.chipName() << " [driver " << target.driver->id() << ", "
              << programmer.eepromSize() << " byte EEPROM]\n";
}

/// The gate that protects against writing an image we do not fully understand:
/// if the checksum of what is already on the chip does not match what this tool
/// computes, our idea of the layout is wrong.
void requireUnderstoodImage(const Programmer& programmer, std::span<const uint8_t> image,
                            const Options& options) {
    if (programmer.verifyChecksum(image)) {
        return;
    }
    if (options.force) {
        std::cerr << "warning: the EEPROM currently on the chip fails its checksum check; "
                     "continuing because --force was given\n";
        return;
    }
    throw Error("the EEPROM currently on the chip fails its checksum check.\n"
                "That means either the chip is blank or erased, or this tool does not model its "
                "layout correctly. Writing now could produce an image the chip rejects.\n"
                "Use 'usbprog dump' to inspect it, restore a known good image with 'usbprog "
                "write', or repeat the command with --force if you know what you are doing.");
}

/// The second half of the blank-identity guard.
///
/// Seeding from the descriptor fixes a chip that still runs on its built-in
/// defaults, but not one already enumerating as ffff:ffff — there the
/// descriptor reports the bad value too. Committing that would stamp a valid
/// checksum onto an identity no probe can match, stranding the device behind
/// --driver for good. It is never what anyone means, so it is refused.
void requireUsableIdentity(const PropertyMap& values, const Options& options) {
    static constexpr uint32_t kBlankId = 0xFFFF;
    for (const char* name : {"vendor_id", "product_id"}) {
        const auto it = values.find(name);
        if (it == values.end() || getNumber(values, name) != kBlankId) {
            continue;
        }
        if (options.force) {
            std::cerr << "warning: writing " << name
                      << "=0xffff; the device will not be found by probing after this, only "
                         "with --device and --driver\n";
            return;
        }
        throw Error(std::string(name) +
                    " is 0xffff, which is what a blank EEPROM reads rather than a real USB ID.\n"
                    "Writing it makes the device impossible to find without --device and "
                    "--driver.\n"
                    "Name the real value (for example vendor_id=0x0403 product_id=0x6011), or "
                    "repeat with --force if you truly want it.");
    }
}

void saveBackup(const Target& target, std::span<const uint8_t> image, const Options& options) {
    if (options.noBackup) {
        return;
    }
    const std::string path = options.backupFile.empty()
                                 ? defaultBackupName(target.info.vendorId, target.info.productId)
                                 : options.backupFile;
    writeBinaryFile(path, image);
    std::cout << "Backup    saved to " << path << "\n";
}

int commandList(const usb::Context& context, const Registry& registry, const Options& options) {
    std::size_t shown = 0;
    for (const usb::Device& device : context.devices()) {
        usb::DeviceInfo info;
        try {
            info = device.info();
        } catch (const Error&) {
            continue;
        }
        if (!options.filter.matches(info)) {
            continue;
        }
        const Driver* driver = registry.probe(info);
        if (driver == nullptr && !options.showAll) {
            continue;
        }
        if (shown == 0) {
            std::cout << "bus:addr  vid:pid    driver    device\n";
        }
        std::cout << describe(info, driver) << "\n";
        ++shown;
    }
    if (shown == 0) {
        if (options.showAll) {
            std::cout << "No USB devices found.\n";
        } else {
            std::cout << "No supported devices found. Use --all to list every USB device.\n";
        }
    }
    return 0;
}

int commandDrivers(const Registry& registry) {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto& driver : registry.drivers()) {
        rows.emplace_back(driver->id(), driver->description());
    }
    std::cout << "Supported chip families:\n";
    printTable(std::cout, rows);
    return 0;
}

void printProperties(const std::vector<PropertySpec>& specs) {
    for (const PropertySpec& spec : specs) {
        std::string type;
        switch (spec.type) {
        case PropertyType::Bool:
            type = "on|off";
            break;
        case PropertyType::Number:
            type = "number " + std::to_string(spec.minimum) + "-" + std::to_string(spec.maximum);
            if (spec.step > 1) {
                type += " step " + std::to_string(spec.step);
            }
            break;
        case PropertyType::String:
            type = "text";
            if (spec.maxChars != 0) {
                type += " (" + std::to_string(spec.maxChars) + " characters shared by all strings)";
            }
            break;
        case PropertyType::Enum:
            type = "one of:";
            break;
        }
        std::cout << "  " << padRight(spec.name, 20) << spec.help << "\n";
        std::cout << "  " << padRight("", 20) << type << "\n";
        if (spec.type == PropertyType::Enum) {
            std::string line;
            for (const EnumValue& value : spec.values) {
                if (line.size() + value.name.size() > 52) {
                    std::cout << "  " << padRight("", 22) << line << "\n";
                    line.clear();
                }
                line += (line.empty() ? "" : " ") + value.name;
            }
            if (!line.empty()) {
                std::cout << "  " << padRight("", 22) << line << "\n";
            }
        }
    }
}

int commandProperties(const usb::Context& context, const Registry& registry,
                      const Options& options) {
    if (!options.driverId.empty()) {
        const Driver* driver = registry.byId(options.driverId);
        if (driver == nullptr) {
            throw Error("unknown driver '" + options.driverId +
                        "'; run 'usbprog drivers' for the list");
        }
        std::cout << "Settings understood by " << driver->id() << ":\n";
        printProperties(driver->properties());
        return 0;
    }

    Attachment attachment = attach(context, registry, options);
    std::cout << "Settings understood by " << attachment.programmer->chipName() << ":\n";
    printProperties(attachment.programmer->properties());
    return 0;
}

int commandInfo(const usb::Context& context, const Registry& registry, const Options& options) {
    Attachment attachment = attach(context, registry, options);
    const std::vector<uint8_t> image = attachment.programmer->readEeprom();

    printDeviceHeader(attachment.target, *attachment.programmer);
    const char* checksumState =
        attachment.programmer->verifyChecksum(image) ? "ok" : "MISMATCH (blank, or unknown layout)";
    std::cout << "Checksum  " << checksumState << "\n\n";

    const PropertyMap values = attachment.programmer->decode(image);
    std::vector<std::pair<std::string, std::string>> rows;
    for (const PropertySpec& spec : attachment.programmer->properties()) {
        const auto it = values.find(spec.name);
        if (it != values.end()) {
            rows.emplace_back(spec.name, format(spec, it->second));
        }
    }
    printTable(std::cout, rows);
    return 0;
}

int commandDump(const usb::Context& context, const Registry& registry, const Options& options) {
    Attachment attachment = attach(context, registry, options);
    const std::vector<uint8_t> image = attachment.programmer->readEeprom();
    hexdump(std::cout, image);
    return 0;
}

int commandRead(const usb::Context& context, const Registry& registry, const Options& options) {
    if (options.outputFile.empty()) {
        throw Error("'read' needs a destination file: usbprog read -o backup.bin");
    }
    Attachment attachment = attach(context, registry, options);
    const std::vector<uint8_t> image = attachment.programmer->readEeprom();
    writeBinaryFile(options.outputFile, image);
    std::cout << "Saved " << image.size() << " bytes from " << attachment.programmer->chipName()
              << " to " << options.outputFile << "\n";
    if (!attachment.programmer->verifyChecksum(image)) {
        std::cerr << "warning: the image fails its checksum check; the chip may be blank\n";
    }
    return 0;
}

int commandWrite(const usb::Context& context, const Registry& registry, const Options& options) {
    if (options.inputFile.empty()) {
        throw Error("'write' needs a source file: usbprog write -i backup.bin");
    }
    const std::vector<uint8_t> image = readBinaryFile(options.inputFile);

    Attachment attachment = attach(context, registry, options);
    Programmer& programmer = *attachment.programmer;
    if (image.size() != programmer.eepromSize()) {
        throw Error("'" + options.inputFile + "' holds " + std::to_string(image.size()) +
                    " bytes, but the " + programmer.chipName() + " EEPROM is " +
                    std::to_string(programmer.eepromSize()) + " bytes");
    }

    printDeviceHeader(attachment.target, programmer);
    if (!programmer.verifyChecksum(image) && !options.force) {
        throw Error("the image in '" + options.inputFile +
                    "' fails its checksum check; the chip would fall back to its factory "
                    "defaults. Repeat with --force to write it anyway.");
    }
    // A saved image can carry a blank identity just as easily as a `set` can
    // produce one, and restoring it strands the device the same way.
    requireUsableIdentity(programmer.decode(image), options);

    if (options.dryRun) {
        std::cout << "Dry run: would write " << image.size() << " bytes from " << options.inputFile
                  << "\n";
        return 0;
    }

    const std::vector<uint8_t> current = programmer.readEeprom();
    if (!confirm("Write " + std::to_string(image.size()) + " bytes to the EEPROM?",
                 options.assumeYes)) {
        std::cout << "Aborted.\n";
        return 1;
    }
    saveBackup(attachment.target, current, options);

    programmer.writeEeprom(image);
    std::cout << "Wrote and verified " << image.size() << " bytes.\n";
    std::cout << "Replug the device for the new settings to take effect.\n";
    return 0;
}

int commandSet(const usb::Context& context, const Registry& registry, const Options& options) {
    if (options.arguments.empty()) {
        throw Error("'set' needs at least one KEY=VALUE pair; run 'usbprog props' for the list");
    }

    Attachment attachment = attach(context, registry, options);
    Programmer& programmer = *attachment.programmer;
    const std::vector<uint8_t> current = programmer.readEeprom();

    printDeviceHeader(attachment.target, programmer);
    requireUnderstoodImage(programmer, current, options);

    const PropertyMap before = programmer.decode(current);
    PropertyMap wanted = before;

    // Before the user's own settings, so anything they name still wins.
    const std::vector<std::string> seeded =
        programmer.seedFromDescriptor(wanted, attachment.target.info);
    for (const std::string& name : seeded) {
        std::cerr << "note: " << name
                  << " read as 0xffff (a blank EEPROM); using the value the device reports over "
                     "USB instead\n";
    }

    for (const std::string& argument : options.arguments) {
        const auto equals = argument.find('=');
        if (equals == std::string::npos) {
            throw Error("'" + argument + "' is not a KEY=VALUE pair");
        }
        const std::string key = text::trim(argument.substr(0, equals));
        const std::string value = argument.substr(equals + 1);
        const PropertySpec* spec = programmer.findProperty(key);
        if (spec == nullptr) {
            throw Error("the " + programmer.chipName() + " has no setting called '" + key +
                        "'; run 'usbprog props' for the list");
        }
        if (!spec->writable) {
            throw Error("'" + key + "' is read-only");
        }
        wanted[key] = parse(*spec, value);
    }

    requireUsableIdentity(wanted, options);

    std::vector<uint8_t> updated = current;
    programmer.encode(wanted, updated);

    // Decode the image we are about to write: what the user sees is what the
    // chip will report, not just what we intended.
    const PropertyMap after = programmer.decode(updated);

    std::vector<std::pair<std::string, std::string>> changes;
    for (const PropertySpec& spec : programmer.properties()) {
        const auto oldValue = before.find(spec.name);
        const auto newValue = after.find(spec.name);
        if (oldValue == before.end() || newValue == after.end() ||
            oldValue->second == newValue->second) {
            continue;
        }
        changes.emplace_back(spec.name, format(spec, oldValue->second) + "  ->  " +
                                            format(spec, newValue->second));
    }

    if (updated == current) {
        std::cout << "Nothing to do: the EEPROM already holds these settings.\n";
        return 0;
    }

    std::cout << "\nChanges:\n";
    if (changes.empty()) {
        std::cout << "  (only bytes without a named setting)\n";
    } else {
        printTable(std::cout, changes);
    }

    std::size_t changedBytes = 0;
    for (std::size_t i = 0; i < updated.size(); ++i) {
        changedBytes += static_cast<std::size_t>(updated[i] != current[i]);
    }
    std::cout << "\n" << changedBytes << " of " << updated.size() << " bytes differ.\n";

    if (options.dryRun) {
        std::cout << "Dry run: nothing was written.\n";
        return 0;
    }

    if (!confirm("Write these changes to the EEPROM?", options.assumeYes)) {
        std::cout << "Aborted.\n";
        return 1;
    }
    saveBackup(attachment.target, current, options);

    programmer.writeEeprom(updated);
    std::cout << "Wrote and verified " << updated.size() << " bytes.\n";
    std::cout << "Replug the device for the new settings to take effect.\n";
    return 0;
}

int commandErase(const usb::Context& context, const Registry& registry, const Options& options) {
    Attachment attachment = attach(context, registry, options);
    Programmer& programmer = *attachment.programmer;
    printDeviceHeader(attachment.target, programmer);

    if (options.dryRun) {
        std::cout << "Dry run: would erase the EEPROM.\n";
        return 0;
    }

    const std::vector<uint8_t> current = programmer.readEeprom();
    if (!confirm("Erase the EEPROM? The chip will revert to its factory defaults.",
                 options.assumeYes)) {
        std::cout << "Aborted.\n";
        return 1;
    }
    saveBackup(attachment.target, current, options);

    programmer.eraseEeprom();
    std::cout << "Erased. Replug the device; it will enumerate with the chip's default IDs.\n";
    return 0;
}

} // namespace

int run(const Options& options) {
    if (options.command == Command::Help) {
        std::cout << usage();
        return 0;
    }
    if (options.command == Command::Version) {
        std::cout << version() << "\n";
        return 0;
    }

    const Registry& registry = Registry::builtin();
    if (options.command == Command::Drivers) {
        return commandDrivers(registry);
    }

    const usb::Context context;
    switch (options.command) {
    case Command::List:
        return commandList(context, registry, options);
    case Command::Properties:
        return commandProperties(context, registry, options);
    case Command::Info:
        return commandInfo(context, registry, options);
    case Command::Dump:
        return commandDump(context, registry, options);
    case Command::Read:
        return commandRead(context, registry, options);
    case Command::Write:
        return commandWrite(context, registry, options);
    case Command::Set:
        return commandSet(context, registry, options);
    case Command::Erase:
        return commandErase(context, registry, options);
    default:
        break;
    }
    throw Error("internal error: unhandled command");
}

} // namespace usbprog::cli
