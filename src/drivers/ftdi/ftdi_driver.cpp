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

#include "drivers/ftdi/ftdi_driver.h"

#include "core/driver.h"
#include "core/error.h"
#include "core/text.h"
#include "drivers/ftdi/ft232r.h"
#include "drivers/ftdi/ftdi_codec.h"
#include "drivers/ftdi/ftx.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace usbprog::ftdi {
namespace {

/// Vendor requests understood by the FTDI USB interface.
constexpr uint8_t kReadEeprom = 0x90;
constexpr uint8_t kWriteEeprom = 0x91;
constexpr uint8_t kEraseEeprom = 0x92;

constexpr int kInterfaceNumber = 0;

/// EEPROM access for every FTDI chip: word addressed reads and writes over
/// endpoint zero. Only the interpretation of the bytes differs per family, and
/// that lives in the Codec.
class FtdiProgrammer final : public usbprog::Programmer {
public:
    FtdiProgrammer(usb::Handle handle, std::string chipName, std::unique_ptr<Codec> codec,
                   std::vector<std::string> warnings)
        : handle_(std::move(handle)), chipName_(std::move(chipName)), codec_(std::move(codec)),
          warnings_(std::move(warnings)) {}

    std::string chipName() const override { return chipName_; }

    std::size_t eepromSize() const override { return codec_->layout().size; }

    std::vector<uint8_t> readEeprom() override {
        std::vector<uint8_t> image(eepromSize());
        for (std::size_t word = 0; word < image.size() / 2; ++word) {
            handle_.controlIn(kReadEeprom, 0, static_cast<uint16_t>(word),
                              std::span<uint8_t>(image).subspan(word * 2, 2));
        }
        return image;
    }

    void writeEeprom(std::span<const uint8_t> image) override {
        if (image.size() != eepromSize()) {
            throw Error("refusing to write " + std::to_string(image.size()) +
                        " bytes to an EEPROM of " + std::to_string(eepromSize()) + " bytes");
        }
        for (std::size_t word = 0; word < image.size() / 2; ++word) {
            const auto value =
                static_cast<uint16_t>(image[word * 2] | (image[(word * 2) + 1] << 8));
            handle_.controlOut(kWriteEeprom, value, static_cast<uint16_t>(word));
        }
        verifyAgainst(image);
    }

    void eraseEeprom() override { handle_.controlOut(kEraseEeprom, 0, 0); }

    const std::vector<PropertySpec>& properties() const override { return codec_->properties(); }

    PropertyMap decode(std::span<const uint8_t> image) const override {
        return codec_->decode(image);
    }

    void encode(const PropertyMap& values, std::vector<uint8_t>& image) const override {
        codec_->encode(values, image);
    }

    bool verifyChecksum(std::span<const uint8_t> image) const override {
        return codec_->verifyChecksum(image);
    }

    std::vector<std::string> warnings() const override { return warnings_; }

private:
    void verifyAgainst(std::span<const uint8_t> expected) {
        const std::vector<uint8_t> actual = readEeprom();
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (actual[i] != expected[i]) {
                throw Error("write verification failed at offset 0x" +
                            text::hex16(static_cast<uint16_t>(i)) + ": wrote 0x" +
                            text::hex8(expected[i]) + ", read back 0x" + text::hex8(actual[i]) +
                            ". The EEPROM contents are now inconsistent; restore a backup or run "
                            "'usbprog erase' to fall back to the factory defaults");
            }
        }
    }

    usb::Handle handle_;
    std::string chipName_;
    std::unique_ptr<Codec> codec_;
    std::vector<std::string> warnings_;
};

/// One driver instance per chip family. Families differ only in the bcdDevice
/// value FTDI puts in the device descriptor and in the codec they use.
class FtdiDriver final : public Driver {
public:
    using CodecFactory = std::function<std::unique_ptr<Codec>()>;

    FtdiDriver(std::string id, std::string chipName, std::string description, uint16_t bcdDevice,
               CodecFactory factory)
        : id_(std::move(id)), chipName_(std::move(chipName)), description_(std::move(description)),
          bcdDevice_(bcdDevice), factory_(std::move(factory)), prototype_(factory_()) {}

    std::string id() const override { return id_; }
    std::string description() const override { return description_; }

    bool probe(const usb::DeviceInfo& info) const override {
        // FTDI encodes the chip generation in bcdDevice. The vendor ID check
        // keeps unrelated devices that happen to report the same release number
        // out; a device whose vendor ID was reprogrammed can still be reached
        // with --driver.
        return info.vendorId == kVendorId && info.bcdDevice == bcdDevice_;
    }

    std::unique_ptr<usbprog::Programmer> attach(usb::Handle handle,
                                                const usb::DeviceInfo& info) const override {
        (void)info;
        std::vector<std::string> warnings;
        if (!handle.detachKernelDriver(kInterfaceNumber)) {
            warnings.emplace_back("could not detach the kernel driver from interface 0");
        }
        if (!handle.claimInterface(kInterfaceNumber)) {
            // EEPROM access goes through endpoint zero, which usually still
            // works while another driver holds the interface. Say so rather
            // than refusing outright.
            warnings.emplace_back(
                "could not claim interface 0 (another driver is using the device); EEPROM access "
                "over endpoint 0 will still be attempted");
        }
        return std::make_unique<FtdiProgrammer>(std::move(handle), chipName_, factory_(),
                                                std::move(warnings));
    }

    const std::vector<PropertySpec>& properties() const override {
        return prototype_->properties();
    }

private:
    std::string id_;
    std::string chipName_;
    std::string description_;
    uint16_t bcdDevice_;
    CodecFactory factory_;
    std::unique_ptr<Codec> prototype_;
};

} // namespace

void registerDrivers(Registry& registry) {
    registry.add(std::make_unique<FtdiDriver>(
        "ft232r", "FT232R", "FTDI FT232R / FT245R, 128 byte internal EEPROM", 0x0600,
        [] { return std::make_unique<Ft232rCodec>(); }));

    registry.add(std::make_unique<FtdiDriver>(
        "ftx", "FT-X", "FTDI FT-X series (FT230X/FT231X/FT234XD), 256 byte EEPROM [experimental]",
        0x1000, [] { return std::make_unique<FtxCodec>(); }));
}

} // namespace usbprog::ftdi
