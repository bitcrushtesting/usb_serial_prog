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

#include "drivers/ch34x/ch34x_driver.h"

#include "core/driver.h"
#include "core/error.h"
#include "core/text.h"
#include "drivers/ch34x/ch340_config.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace usbprog::ch34x {
namespace {

/// Vendor requests understood by the CH34x USB interface.
///
/// The serial port requests (0x5F, 0x9A, 0xA1, 0xA4) are the ones the Linux
/// ch341 driver uses and are well established. The configuration area request
/// is not: WCH documents the memory in the datasheet but not the way to reach
/// it, so 0x54 and the delay that has to follow a write come from USB captures
/// of WCH's own CH34xSerCfg tool. They are corroborated by the layout —
/// address 0x04 reads back the low byte of the vendor ID, exactly where the
/// datasheet puts it — but this is the part of the driver that rests on
/// observation rather than documentation.
constexpr uint8_t kVersion = 0x5F; ///< chip version, 2 bytes
constexpr uint8_t kSerialInit = 0xA1;
constexpr uint8_t kModemOut = 0xA4;
constexpr uint8_t kConfigAccess = 0x54; ///< configuration area, read and write
constexpr uint8_t kDelay = 0x5E;        ///< wait wValue milliseconds

/// wIndex for every configuration transfer. The 0xA0 half is the I2C address
/// an EEPROM answers to, which is a fair hint at how the block is wired up
/// inside the chip.
constexpr uint16_t kConfigIndex = 0xA001;

/// How long the chip is given to finish an internal write. The captures use
/// ten milliseconds after every byte.
constexpr uint16_t kWriteDelayMs = 10;

constexpr int kInterfaceNumber = 0;

/// Read the two byte chip version. Every CH34x answers this, so a failure here
/// means the device is not reachable at all rather than merely unsupported.
uint8_t readVersion(usb::Handle& handle) {
    std::array<uint8_t, 2> version{};
    handle.controlIn(kVersion, 0, 0, version);
    return version[0];
}

uint8_t readConfigByte(usb::Handle& handle, uint16_t address) {
    std::array<uint8_t, 1> byte{};
    handle.controlIn(kConfigAccess, static_cast<uint16_t>(address << 8), kConfigIndex, byte);
    return byte[0];
}

/// The address and the value share wValue: address in the high byte, the byte
/// to store in the low one. Nothing travels in a data stage.
void writeConfigByte(usb::Handle& handle, uint16_t address, uint8_t value) {
    handle.controlOut(kConfigAccess, static_cast<uint16_t>((address << 8) | value), kConfigIndex);
    handle.controlOut(kDelay, kWriteDelayMs, 0);
}

/// The preamble CH34xSerCfg sends before it touches the configuration area: a
/// serial initialisation followed by two modem control writes. It disturbs the
/// UART state, which is why reads only fall back to it when the plain request
/// is refused, while writes always send it.
void prepare(usb::Handle& handle) {
    handle.controlOut(kSerialInit, 0xC39C, 0xD9E8);
    handle.controlOut(kModemOut, 0x00DF, 0);
    handle.controlOut(kModemOut, 0x009F, 0);
}

/// The CH340B configuration area, reached over endpoint zero one byte at a
/// time. Only the CH340B (and, with a different signature, the CH340H/S) has
/// one; the plain CH340/CH340G refuses the request outright.
class Ch340Programmer final : public usbprog::Programmer {
public:
    Ch340Programmer(usb::Handle handle, std::vector<std::string> warnings)
        : handle_(std::move(handle)), warnings_(std::move(warnings)) {}

    std::string chipName() const override { return "CH340B"; }

    std::size_t eepromSize() const override { return config_.size(); }

    std::vector<uint8_t> readEeprom() override {
        std::vector<uint8_t> image(eepromSize());
        for (std::size_t address = 0; address < image.size(); ++address) {
            image[address] = readConfigByte(handle_, static_cast<uint16_t>(address));
        }
        return image;
    }

    void writeEeprom(std::span<const uint8_t> image) override {
        if (image.size() != eepromSize()) {
            throw Error("refusing to write " + std::to_string(image.size()) +
                        " bytes to a configuration area of " + std::to_string(eepromSize()) +
                        " bytes");
        }
        // The lock byte is a one way door: once the chip reads 0x57 there, no
        // later write is accepted and the identity it holds is final. Nothing
        // sets it deliberately, so an image carrying it is a mistake rather
        // than an instruction.
        if (Ch340Config::isWriteProtected(image)) {
            throw Error("this image sets the write protect byte (0x" +
                        text::hex8(config::kWriteProtected) +
                        " at offset 0x03), which locks the "
                        "configuration area permanently.\n"
                        "Clear that byte before writing the image.");
        }

        const std::vector<uint8_t> current = readEeprom();
        if (Ch340Config::isWriteProtected(current)) {
            throw Error("the configuration area of this chip is write protected (offset 0x03 "
                        "reads 0x" +
                        text::hex8(config::kWriteProtected) +
                        "). The chip enforces that itself; nothing can unlock it.");
        }

        prepare(handle_);

        // The signature is cleared first and restored last. In between the
        // chip ignores the area and runs on its vendor defaults, so a write
        // that is interrupted half way leaves a device that still enumerates
        // rather than one carrying half of an identity.
        writeConfigByte(handle_, addr::kSig, 0x00);
        for (std::size_t address = 1; address < image.size(); ++address) {
            writeConfigByte(handle_, static_cast<uint16_t>(address), image[address]);
        }
        writeConfigByte(handle_, addr::kSig, image[addr::kSig]);

        verifyAgainst(image, current);
    }

    void eraseEeprom() override {
        // Clearing the signature is the documented way back: the chip stops
        // reading the area and falls back to 1a86:7523 with its own strings.
        // The bytes stay behind, which makes this reversible.
        prepare(handle_);
        writeConfigByte(handle_, addr::kSig, 0x00);
        const uint8_t signature = readConfigByte(handle_, addr::kSig);
        if (signature != 0x00) {
            throw Error("erase failed: the signature byte still reads 0x" + text::hex8(signature) +
                        ". The configuration area may be write protected (offset 0x03).");
        }
    }

    const std::vector<PropertySpec>& properties() const override { return config_.properties(); }

    PropertyMap decode(std::span<const uint8_t> image) const override {
        return config_.decode(image);
    }

    void encode(const PropertyMap& values, std::vector<uint8_t>& image) const override {
        config_.encode(values, image);
    }

    bool verifyChecksum(std::span<const uint8_t> image) const override {
        return config_.verifyChecksum(image);
    }

    /// A CH340B that is running on its defaults decodes to those defaults
    /// rather than to a blank identity, so there is normally nothing to seed.
    /// An area that was written by something else can still hold 0xffff, and
    /// that is worth replacing with what the device reports.
    std::vector<std::string> seedFromDescriptor(PropertyMap& values,
                                                const usb::DeviceInfo& info) const override {
        struct Seed {
            const char* name;
            uint16_t reported;
        };
        const Seed seeds[] = {{"vendor_id", info.vendorId}, {"product_id", info.productId}};

        std::vector<std::string> filled;
        for (const Seed& seed : seeds) {
            const auto it = values.find(seed.name);
            if (it == values.end() || getNumber(values, seed.name) != kBlankId) {
                continue;
            }
            if (seed.reported == kBlankId || seed.reported == 0) {
                continue;
            }
            it->second = static_cast<uint32_t>(seed.reported);
            filled.emplace_back(seed.name);
        }
        return filled;
    }

    std::vector<std::string> warnings() const override { return warnings_; }

private:
    /// What a blank field reads as. The CH340B treats it as "use the vendor
    /// default" rather than as an address, so it is never a usable identity.
    static constexpr uint32_t kBlankId = 0xFFFF;

    void verifyAgainst(std::span<const uint8_t> expected, std::span<const uint8_t> before) {
        const std::vector<uint8_t> actual = readEeprom();

        // Every write was accepted and the area came back exactly as it went
        // in. Nothing is half written, so the chip is not damaged: the
        // configuration interface simply did not do anything, and telling the
        // user to restore a backup would send them after a problem they do
        // not have.
        if (std::equal(actual.begin(), actual.end(), before.begin(), before.end()) &&
            !std::equal(actual.begin(), actual.end(), expected.begin(), expected.end())) {
            throw Error("the chip accepted every write and stored none of them: the configuration "
                        "area reads exactly as it did before.\n"
                        "Nothing was changed, so the chip is unharmed and needs no recovery. What "
                        "this means is that the way this driver reaches the configuration area "
                        "(vendor request 0x" +
                        text::hex8(kConfigAccess) +
                        ") is not how this particular part expects to be addressed. That request "
                        "is not documented by WCH; see the CH340 section of the README.");
        }

        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (actual[i] != expected[i]) {
                throw Error("write verification failed at offset 0x" +
                            text::hex16(static_cast<uint16_t>(i)) + ": wrote 0x" +
                            text::hex8(expected[i]) + ", read back 0x" + text::hex8(actual[i]) +
                            ".\nThe configuration area is now inconsistent. Restore the backup "
                            "this command saved with 'usbprog write -i BACKUP', or run 'usbprog "
                            "erase' to fall back to the factory defaults.");
            }
        }
    }

    usb::Handle handle_;
    std::vector<std::string> warnings_;
    Ch340Config config_;
};

class Ch340Driver final : public Driver {
public:
    std::string id() const override { return "ch340"; }

    std::string description() const override {
        return "WCH CH340B, 64 byte on-chip configuration area [experimental]";
    }

    bool probe(const usb::DeviceInfo& info) const override {
        // WCH does not distinguish the variants in the descriptor: a CH340B
        // and a CH340G both enumerate as 1a86:7523. Whether this particular
        // chip has a configuration area is settled in attach(), by asking it.
        return info.vendorId == kVendorId &&
               (info.productId == 0x7522 || info.productId == 0x7523 || info.productId == 0x5523);
    }

    std::unique_ptr<usbprog::Programmer> attach(usb::Handle handle, const usb::DeviceInfo& info,
                                                const AttachOptions& options) const override {
        (void)info;
        (void)options; // the area is on die, so its size is not in question
        std::vector<std::string> warnings;
        if (!handle.detachKernelDriver(kInterfaceNumber)) {
            warnings.emplace_back("could not detach the kernel driver from interface 0");
        }
        if (!handle.claimInterface(kInterfaceNumber)) {
            // Configuration access goes through endpoint zero, which usually
            // still works while another driver holds the interface.
            warnings.emplace_back(
                "could not claim interface 0 (another driver is using the device); configuration "
                "access over endpoint 0 will still be attempted");
        }

        const uint8_t signature = readSignature(handle);
        if (isUniform(handle, signature)) {
            // A blank area and a configuration interface this driver cannot
            // reach look exactly alike from here, and on the one CH340B this
            // was tried against they were not distinguishable by reading at
            // all: every write was accepted and none of them took effect.
            // Saying so beats printing a confident table of defaults.
            warnings.emplace_back(
                "the configuration area reads 0x" + text::hex8(signature) +
                " at every address. That is what a blank chip looks like, and also what a chip "
                "whose configuration interface this driver cannot reach looks like. The settings "
                "below are the vendor defaults the chip falls back on, not values read from it. "
                "A write will be read back and will fail loudly if it does not land");
        }
        if (signature == config::kSigExternal) {
            throw Error("this chip answers with the CH340H/CH340S signature (0x" +
                        text::hex8(config::kSigExternal) +
                        "), whose configuration lives in an external part and follows a different "
                        "layout.\nThis driver only writes the CH340B on-chip area, so it stops "
                        "here rather than write the wrong bytes.");
        }
        return std::make_unique<Ch340Programmer>(std::move(handle), std::move(warnings));
    }

    const std::vector<PropertySpec>& properties() const override { return prototype_.properties(); }

private:
    /// Ask the chip whether it has a configuration area at all, and leave the
    /// serial state alone unless it turns out that the preamble is needed.
    ///
    /// A CH340 without the memory does not answer with zeroes or with an empty
    /// area: it stalls the request. That is the only reliable way to tell the
    /// variants apart, because the version byte does not distinguish them.
    static uint8_t readSignature(usb::Handle& handle) {
        try {
            return readConfigByte(handle, addr::kSig);
        } catch (const UsbError&) {
            // Fall through to the retry below.
        }

        try {
            prepare(handle);
            return readConfigByte(handle, addr::kSig);
        } catch (const UsbError&) {
            throw Error(describeMissingConfigArea(handle));
        }
    }

    /// Whether every address returns the same byte the signature did. Reading
    /// the area twice at attach is cheap next to what it buys: the CLI prints
    /// its warnings before any of the values.
    static bool isUniform(usb::Handle& handle, uint8_t signature) {
        for (std::size_t address = 1; address < config::kBytes; ++address) {
            if (readConfigByte(handle, static_cast<uint16_t>(address)) != signature) {
                return false;
            }
        }
        return true;
    }

    static std::string describeMissingConfigArea(usb::Handle& handle) {
        std::string version;
        try {
            version = " (it reports chip version 0x" + text::hex8(readVersion(handle)) + ")";
        } catch (const Error&) {
            // The version is a nicety; its absence must not mask the real
            // diagnosis below.
        }
        return "this chip refuses the configuration request" + version +
               ", so it has no settings to write.\n"
               "Only the CH340B has a configuration area. The CH340, CH340G, CH340C, CH340N and "
               "the rest of the family hold their USB identity in mask ROM, which fixes them at "
               "1a86:7523 for good; no tool can change that.\n"
               "If you meant to program a CH340B, check that the chip on the board really is the "
               "B variant.";
    }

    Ch340Config prototype_;
};

} // namespace

void registerDrivers(Registry& registry) { registry.add(std::make_unique<Ch340Driver>()); }

} // namespace usbprog::ch34x
