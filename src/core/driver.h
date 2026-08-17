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

#include "core/property.h"
#include "core/usb.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace usbprog {

/// A device that has been opened and identified. One instance talks to one
/// chip; it is the only place that knows how that chip stores its settings.
///
/// The contract for adding a chip family is small on purpose:
///   * readEeprom/writeEeprom move raw bytes,
///   * decode/encode translate between raw bytes and named properties,
///   * properties() advertises what can be named.
/// Everything the CLI prints and parses follows from those.
class Programmer {
public:
    Programmer() = default;
    virtual ~Programmer() = default;

    // A programmer owns an open device; it is held by pointer and never copied.
    Programmer(const Programmer&) = delete;
    Programmer& operator=(const Programmer&) = delete;
    Programmer(Programmer&&) = delete;
    Programmer& operator=(Programmer&&) = delete;

    /// Marketing name of the detected chip, e.g. "FT232R".
    virtual std::string chipName() const = 0;

    /// Size of the configuration memory in bytes.
    virtual std::size_t eepromSize() const = 0;

    virtual std::vector<uint8_t> readEeprom() = 0;

    /// Write the whole image. Implementations verify by reading back.
    virtual void writeEeprom(std::span<const uint8_t> image) = 0;

    /// Erase to the blank state. The chip then falls back to its factory
    /// defaults, which is also the recovery path from a bad image.
    virtual void eraseEeprom() = 0;

    virtual const std::vector<PropertySpec>& properties() const = 0;

    /// Interpret an image. Every writable property appears in the result.
    virtual PropertyMap decode(std::span<const uint8_t> image) const = 0;

    /// Apply a full property map to an existing image, in place. Bytes the
    /// driver does not understand are left untouched, so a modified image keeps
    /// factory calibration and reserved fields intact. Recomputes the checksum.
    virtual void encode(const PropertyMap& values, std::vector<uint8_t>& image) const = 0;

    /// True when the image's stored checksum matches the computed one. The CLI
    /// uses this as a self-test before writing: if we cannot reproduce the
    /// checksum of the image already on the chip, we do not understand its
    /// layout well enough to modify it.
    virtual bool verifyChecksum(std::span<const uint8_t> image) const = 0;

    /// Fill in properties that decoded as the blank-EEPROM placeholder with
    /// what the device actually reports over USB.
    ///
    /// A blank or damaged EEPROM decodes its identity as 0xffff. Because `set`
    /// is a read-modify-write, naming only the strings on such a chip commits
    /// that 0xffff along with a freshly computed — and therefore valid —
    /// checksum, and the chip enumerates as ffff:ffff, where no probe can find
    /// it. Seeding from the descriptor closes that, since a chip running on
    /// its built-in defaults still reports the right IDs.
    ///
    /// Only blank values are touched; anything the user names is applied after
    /// this and wins. Returns the properties filled in, so the CLI can say so.
    virtual std::vector<std::string> seedFromDescriptor(PropertyMap& values,
                                                        const usb::DeviceInfo& info) const {
        (void)values;
        (void)info;
        return {};
    }

    /// Non-fatal problems noticed while attaching, e.g. a kernel driver we
    /// could not detach. The CLI shows these to the user.
    virtual std::vector<std::string> warnings() const { return {}; }

    const PropertySpec* findProperty(std::string_view name) const;
};

/// Anything the CLI knows that a driver cannot work out for itself.
struct AttachOptions {
    /// Size of an external configuration EEPROM in bytes, 0 to measure it.
    /// Only needed when measuring is impossible, which is the case for a blank
    /// EEPROM: chips that wrap addresses read the same at every size when
    /// empty, and writing the wrong size destroys the image.
    std::size_t eepromBytes = 0;
};

/// Recognises a chip family and creates Programmers for it.
class Driver {
public:
    Driver() = default;
    virtual ~Driver() = default;

    // Drivers live in the registry for the lifetime of the process.
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver(Driver&&) = delete;
    Driver& operator=(Driver&&) = delete;

    /// Short identifier used with --driver, e.g. "ft232r".
    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    /// True when this driver can talk to the device.
    virtual bool probe(const usb::DeviceInfo& info) const = 0;

    virtual std::unique_ptr<Programmer> attach(usb::Handle handle, const usb::DeviceInfo& info,
                                               const AttachOptions& options) const = 0;

    /// Property schema without a device attached, so `usbprog props --driver X`
    /// works with nothing plugged in.
    virtual const std::vector<PropertySpec>& properties() const = 0;
};

} // namespace usbprog
