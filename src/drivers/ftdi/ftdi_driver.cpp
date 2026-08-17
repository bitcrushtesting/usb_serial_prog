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
#include "drivers/ftdi/ft4232h.h"
#include "drivers/ftdi/ftdi_codec.h"
#include "drivers/ftdi/ftx.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace usbprog::ftdi {
namespace {

/// Vendor requests understood by the FTDI USB interface.
constexpr uint8_t kReset = 0x00;
constexpr uint8_t kPollModemStatus = 0x05;
constexpr uint8_t kSetLatencyTimer = 0x09;
constexpr uint8_t kReadEeprom = 0x90;
constexpr uint8_t kWriteEeprom = 0x91;
constexpr uint8_t kEraseEeprom = 0x92;

constexpr uint16_t kResetSio = 0; ///< wValue for a full reset

/// wIndex for the port-level requests. FTDI numbers channels from one, so the
/// first channel is 1 even on single channel parts.
constexpr uint16_t kPortIndex = 1;

/// Latency FTDI's MProg sets before programming. The value matters less than
/// issuing the request at all; it is what the traced sequence uses.
constexpr uint16_t kProgrammingLatency = 0x77;

constexpr int kInterfaceNumber = 0;

/// Smallest part worth considering: a 93C46 holds 64 words.
constexpr std::size_t kSmallestEeprom = 128;

/// Read `bytes` bytes of the EEPROM window, one word per control transfer.
std::vector<uint8_t> readWindow(usb::Handle& handle, std::size_t bytes) {
    std::vector<uint8_t> image(bytes);
    for (std::size_t word = 0; word < bytes / 2; ++word) {
        handle.controlIn(kReadEeprom, 0, static_cast<uint16_t>(word),
                         std::span<uint8_t>(image).subspan(word * 2, 2));
    }
    return image;
}

/// Measure the part actually fitted.
///
/// FTDI chips wrap EEPROM word addresses modulo the size of the part, so
/// reading a window larger than the EEPROM returns its contents mirrored. Each
/// mirror that repeats exactly halves the answer.
///
/// Returns 0 when the size cannot be told, which happens whenever the contents
/// are uniform: a blank EEPROM reads 0xFF at every address and so mirrors
/// identically at every size. Guessing there is what wraps a too-large image
/// onto the header and destroys it, so the caller must ask instead.
std::size_t detectEepromBytes(std::span<const uint8_t> window) {
    const bool uniform =
        std::all_of(window.begin(), window.end(), [&](uint8_t b) { return b == window[0]; });
    if (uniform) {
        return 0;
    }

    std::size_t size = window.size();
    while (size / 2 >= kSmallestEeprom &&
           std::equal(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(size / 2),
                      window.begin() + static_cast<std::ptrdiff_t>(size / 2),
                      window.begin() + static_cast<std::ptrdiff_t>(size))) {
        size /= 2;
    }
    return size;
}

/// EEPROM access for every FTDI chip: word addressed reads and writes over
/// endpoint zero. Only the interpretation of the bytes differs per family, and
/// that lives in the Codec.
class FtdiProgrammer final : public usbprog::Programmer {
public:
    FtdiProgrammer(usb::Handle handle, std::string chipName, std::unique_ptr<Codec> codec,
                   std::vector<std::string> warnings, std::size_t writableBytes,
                   bool eraseSupported)
        : handle_(std::move(handle)), chipName_(std::move(chipName)), codec_(std::move(codec)),
          warnings_(std::move(warnings)), writableBytes_(writableBytes),
          eraseSupported_(eraseSupported) {}

    std::string chipName() const override { return chipName_; }

    std::size_t eepromSize() const override { return codec_->layout().size; }

    std::vector<uint8_t> readEeprom() override { return readWindow(handle_, eepromSize()); }

    void writeEeprom(std::span<const uint8_t> image) override {
        if (image.size() != eepromSize()) {
            throw Error("refusing to write " + std::to_string(image.size()) +
                        " bytes to an EEPROM of " + std::to_string(eepromSize()) + " bytes");
        }
        // The chip wraps word addresses modulo the part fitted, so an image
        // larger than the EEPROM does not overflow harmlessly: its upper half
        // lands back on the header and destroys it. Nothing downstream can
        // recover from that, so it is checked here rather than trusted, and an
        // unknown size is treated as unsafe rather than assumed to be fine.
        if (writableBytes_ == 0) {
            throw Error("the size of the EEPROM fitted to this chip could not be measured, "
                        "because a blank EEPROM reads the same at every size. Writing the wrong "
                        "size wraps the image onto its own header and destroys it.\n"
                        "State the part with --eeprom-size 128 (93C46) or --eeprom-size 256 "
                        "(93C56, 93C66).");
        }
        if (image.size() > writableBytes_) {
            throw Error("refusing to write " + std::to_string(image.size()) + " bytes to the " +
                        std::to_string(writableBytes_) +
                        " byte EEPROM fitted to this chip: the chip wraps addresses, so the upper "
                        "half of the image would overwrite the header.");
        }
        prepareForWrite();
        for (std::size_t word = 0; word < image.size() / 2; ++word) {
            const auto value =
                static_cast<uint16_t>(image[word * 2] | (image[(word * 2) + 1] << 8));
            handle_.controlOut(kWriteEeprom, value, static_cast<uint16_t>(word));
        }
        verifyAgainst(image);
    }

    void eraseEeprom() override {
        // FTDI's MProg manual states the erase request is not supported on the
        // FT232R/FT245R, and the FT-X stores its settings in on-die MTP rather
        // than an erasable EEPROM. Sending it anyway does nothing useful and
        // invites the belief that the chip has been reset when it has not.
        if (!eraseSupported_) {
            throw Error(chipName_ +
                        " does not support the erase request; its configuration memory is not "
                        "erasable that way.\n"
                        "To go back to a known state, write an image you saved earlier with "
                        "'usbprog write -i BACKUP'.");
        }
        handle_.controlOut(kEraseEeprom, 0, 0);
    }

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

    std::vector<std::string> seedFromDescriptor(PropertyMap& values,
                                                const usb::DeviceInfo& info) const override {
        return codec_->seedIdentity(values, info.vendorId, info.productId);
    }

    std::vector<std::string> warnings() const override { return warnings_; }

private:
    /// Put the port into the state FTDI's own programming tool leaves it in
    /// before it touches the EEPROM: reset the SIO block, read the modem
    /// status, then set the programming latency.
    ///
    /// This is not ceremony. Without it an FT232R accepts the first word or
    /// two and then silently drops every write that follows, leaving a mostly
    /// erased EEPROM behind. The sequence was traced from MProg and is what
    /// libftdi's ftdi_write_eeprom() does for the same reason.
    void prepareForWrite() {
        handle_.controlOut(kReset, kResetSio, kPortIndex);
        std::array<uint8_t, 2> modemStatus{};
        handle_.controlIn(kPollModemStatus, 0, kPortIndex, modemStatus);
        handle_.controlOut(kSetLatencyTimer, kProgrammingLatency, kPortIndex);
    }

    void verifyAgainst(std::span<const uint8_t> expected) {
        const std::vector<uint8_t> actual = readEeprom();
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (actual[i] != expected[i]) {
                throw Error("write verification failed at offset 0x" +
                            text::hex16(static_cast<uint16_t>(i)) + ": wrote 0x" +
                            text::hex8(expected[i]) + ", read back 0x" + text::hex8(actual[i]) +
                            ".\nThe EEPROM contents are now inconsistent. Restore the backup this "
                            "command saved with 'usbprog write -i BACKUP'" +
                            (eraseSupported_ ? ", or run 'usbprog erase' to fall back to the "
                                               "factory defaults"
                                             : "") +
                            ".");
            }
        }
    }

    usb::Handle handle_;
    std::string chipName_;
    std::unique_ptr<Codec> codec_;
    std::vector<std::string> warnings_;
    /// How many bytes it is safe to write. 0 means the size is unknown, which
    /// bars writing altogether rather than defaulting to the nominal size.
    std::size_t writableBytes_ = 0;
    /// Whether the chip honours the erase request at all.
    bool eraseSupported_ = false;
};

/// One driver instance per chip family. Families differ only in the bcdDevice
/// value FTDI puts in the device descriptor and in the codec they use.
class FtdiDriver final : public Driver {
public:
    /// Builds a codec for an EEPROM of the given size. Families with on-die
    /// memory ignore the argument; the FT4232H has an external part whose size
    /// changes the layout.
    using CodecFactory = std::function<std::unique_ptr<Codec>(std::size_t)>;

    FtdiDriver(std::string id, std::string chipName, std::string description, uint16_t bcdDevice,
               std::size_t nominalBytes, bool externalEeprom, bool eraseSupported,
               CodecFactory factory)
        : id_(std::move(id)), chipName_(std::move(chipName)), description_(std::move(description)),
          bcdDevice_(bcdDevice), nominalBytes_(nominalBytes), externalEeprom_(externalEeprom),
          eraseSupported_(eraseSupported), factory_(std::move(factory)),
          prototype_(factory_(nominalBytes)) {}

    std::string id() const override { return id_; }
    std::string description() const override { return description_; }

    bool probe(const usb::DeviceInfo& info) const override {
        // FTDI encodes the chip generation in bcdDevice. The vendor ID check
        // keeps unrelated devices that happen to report the same release number
        // out; a device whose vendor ID was reprogrammed can still be reached
        // with --driver.
        return info.vendorId == kVendorId && info.bcdDevice == bcdDevice_;
    }

    std::unique_ptr<usbprog::Programmer> attach(usb::Handle handle, const usb::DeviceInfo& info,
                                                const AttachOptions& options) const override {
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

        const EepromSize size = resolveEepromSize(handle, options, warnings);
        return std::make_unique<FtdiProgrammer>(std::move(handle), chipName_, factory_(size.layout),
                                                std::move(warnings), size.physical,
                                                eraseSupported_);
    }

    const std::vector<PropertySpec>& properties() const override {
        return prototype_->properties();
    }

private:
    struct EepromSize {
        std::size_t layout = 0;   ///< size the codec should model
        std::size_t physical = 0; ///< size measured on the chip, 0 when unknown
    };

    /// Decide how big the EEPROM is before anything is written to it.
    ///
    /// On-die memory is always the nominal size and needs no thought. An
    /// external part is whatever the board maker fitted, and getting it wrong
    /// is destructive rather than merely wrong: the chip wraps addresses, so a
    /// too-large image overwrites its own header. So the size is measured, and
    /// when it cannot be measured the user is asked rather than guessed at.
    EepromSize resolveEepromSize(usb::Handle& handle, const AttachOptions& options,
                                 std::vector<std::string>& warnings) const {
        if (!externalEeprom_) {
            return EepromSize{nominalBytes_, nominalBytes_};
        }

        const std::size_t measured = detectEepromBytes(readWindow(handle, nominalBytes_));

        if (options.eepromBytes != 0) {
            if (measured != 0 && measured != options.eepromBytes) {
                throw Error("--eeprom-size says " + std::to_string(options.eepromBytes) +
                            " bytes, but this chip measures " + std::to_string(measured) +
                            " bytes. Drop the option to use the measured size.");
            }
            return EepromSize{options.eepromBytes, options.eepromBytes};
        }

        if (measured != 0) {
            return EepromSize{measured, measured};
        }

        // Uniform contents, so the mirror test tells us nothing. Reading and
        // decoding still work at the nominal size; only writing is unsafe, and
        // FtdiProgrammer refuses that with physical == 0 left unset below.
        warnings.emplace_back(
            "the EEPROM is blank, so its size cannot be measured (a 93C46 and a 93C66 read alike "
            "when empty); assuming " +
            std::to_string(nominalBytes_) +
            " bytes. Pass --eeprom-size to state it before writing");
        return EepromSize{nominalBytes_, 0};
    }

    std::string id_;
    std::string chipName_;
    std::string description_;
    uint16_t bcdDevice_;
    std::size_t nominalBytes_;
    bool externalEeprom_;
    bool eraseSupported_;
    CodecFactory factory_;
    std::unique_ptr<Codec> prototype_;
};

} // namespace

void registerDrivers(Registry& registry) {
    registry.add(std::make_unique<FtdiDriver>(
        "ft232r", "FT232R", "FTDI FT232R / FT245R, 128 byte internal EEPROM", 0x0600, 128, false,
        false, [](std::size_t) { return std::make_unique<Ft232rCodec>(); }));

    registry.add(std::make_unique<FtdiDriver>(
        "ftx", "FT-X", "FTDI FT-X series (FT230X/FT231X/FT234XD), 256 byte EEPROM [experimental]",
        0x1000, 256, false, false, [](std::size_t) { return std::make_unique<FtxCodec>(); }));

    registry.add(std::make_unique<FtdiDriver>(
        "ft4232h", "FT4232H",
        "FTDI FT4232H quad UART, external 93C46/93C56/93C66 EEPROM [experimental]", 0x0800, 256,
        true, true, [](std::size_t bytes) { return std::make_unique<Ft4232hCodec>(bytes); }));
}

} // namespace usbprog::ftdi
