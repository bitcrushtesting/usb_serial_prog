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

// EEPROM encoding tests. These need no hardware: they exercise the codecs,
// which is where a mistake would be written to somebody's chip.

#include "core/error.h"
#include "core/property.h"
#include "core/registry.h"
#include "core/text.h"
#include "drivers/ftdi/ft232r.h"
#include "drivers/ftdi/ft4232h.h"
#include "drivers/ftdi/ftdi_codec.h"
#include "drivers/ftdi/ftx.h"

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL (line " << line << "): " << what << "\n";
    }
}

template<typename A, typename B>
void checkEqual(const A& actual, const B& expected, const std::string& what, int line) {
    ++checks;
    if (!(actual == expected)) {
        ++failures;
        std::cerr << "FAIL (line " << line << "): " << what << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)
#define CHECK_EQUAL(actual, expected) checkEqual((actual), (expected), #actual, __LINE__)

using namespace usbprog;

/// A blank image decoded and re-encoded, which is the starting point for most
/// of the tests below.
std::vector<uint8_t> blankImage(const ftdi::Codec& codec) {
    // Braces here would build a two element vector rather than one of
    // codec.layout().size bytes.
    // NOLINTNEXTLINE(modernize-return-braced-init-list)
    return std::vector<uint8_t>(codec.layout().size, 0x00);
}

void testChecksumOfZeroImage() {
    // The FTDI checksum seeds with 0xAAAA and rotates left once per word. With
    // every word zero the XOR does nothing, so 63 rotations of 0xAAAA remain,
    // which is the same as rotating right once: 0x5555.
    const ftdi::Ft232rCodec codec;
    const std::vector<uint8_t> image = blankImage(codec);
    CHECK_EQUAL(codec.computeChecksum(image), 0x5555);
}

void testChecksumIsStoredInTheLastWord() {
    const ftdi::Ft232rCodec codec;
    std::vector<uint8_t> image = blankImage(codec);
    const PropertyMap values = codec.decode(image);
    codec.encode(values, image);

    CHECK(codec.verifyChecksum(image));
    const auto stored = static_cast<uint16_t>(image[0x7E] | (image[0x7F] << 8));
    CHECK_EQUAL(stored, codec.computeChecksum(image));

    // Flipping any byte in the covered area invalidates it.
    image[0x20] ^= 0xFF;
    CHECK(!codec.verifyChecksum(image));
}

void testIdentifiersAndPower() {
    const ftdi::Ft232rCodec codec;
    std::vector<uint8_t> image = blankImage(codec);
    PropertyMap values = codec.decode(image);
    values["vendor_id"] = uint32_t{0x0403};
    values["product_id"] = uint32_t{0x6001};
    values["max_power"] = uint32_t{100};
    values["self_powered"] = true;
    values["remote_wakeup"] = false;
    values["use_serial"] = true;
    values["suspend_pull_downs"] = true;
    codec.encode(values, image);

    CHECK_EQUAL(int(image[0x02]), 0x03); // little endian vendor ID
    CHECK_EQUAL(int(image[0x03]), 0x04);
    CHECK_EQUAL(int(image[0x04]), 0x01);
    CHECK_EQUAL(int(image[0x05]), 0x60);
    CHECK_EQUAL(int(image[0x09]), 50);          // 100 mA in 2 mA units
    CHECK_EQUAL(int(image[0x08] & 0x80), 0x80); // reserved bit always set
    CHECK_EQUAL(int(image[0x08] & 0x40), 0x40); // self powered
    CHECK_EQUAL(int(image[0x08] & 0x20), 0x00); // no remote wakeup
    CHECK_EQUAL(int(image[0x0A] & 0x08), 0x08); // use serial
    CHECK_EQUAL(int(image[0x0A] & 0x04), 0x04); // suspend pull downs

    const PropertyMap back = codec.decode(image);
    CHECK_EQUAL(getNumber(back, "vendor_id"), 0x0403u);
    CHECK_EQUAL(getNumber(back, "product_id"), 0x6001u);
    CHECK_EQUAL(getNumber(back, "max_power"), 100u);
    CHECK(getBool(back, "self_powered"));
    CHECK(!getBool(back, "remote_wakeup"));
    CHECK(getBool(back, "use_serial"));
}

void testStringRoundTrip() {
    const ftdi::Ft232rCodec codec;
    std::vector<uint8_t> image = blankImage(codec);
    PropertyMap values = codec.decode(image);
    values["manufacturer"] = std::string("ACME");
    values["product"] = std::string("Widget Brücke"); // non-ASCII on purpose
    values["serial_number"] = std::string("WB0001");
    codec.encode(values, image);

    const PropertyMap back = codec.decode(image);
    CHECK_EQUAL(getString(back, "manufacturer"), std::string("ACME"));
    CHECK_EQUAL(getString(back, "product"), std::string("Widget Brücke"));
    CHECK_EQUAL(getString(back, "serial_number"), std::string("WB0001"));

    // Strings start at 0x18 and are stored as USB string descriptors: a length
    // byte, the descriptor type 0x03, then UTF-16LE.
    CHECK_EQUAL(int(image[0x0E]), 0x18 | 0x80);
    CHECK_EQUAL(int(image[0x0F]), 2 + (4 * 2));
    CHECK_EQUAL(int(image[0x18]), 2 + (4 * 2));
    CHECK_EQUAL(int(image[0x19]), 0x03);
    CHECK_EQUAL(int(image[0x1A]), 'A');
    CHECK_EQUAL(int(image[0x1B]), 0x00);

    // The product string follows immediately after the manufacturer string.
    CHECK_EQUAL(int(image[0x10]), (0x18 + 10) | 0x80);
}

void testStringsThatDoNotFitAreRejected() {
    const ftdi::Ft232rCodec codec;
    std::vector<uint8_t> image = blankImage(codec);
    PropertyMap values = codec.decode(image);
    values["manufacturer"] = std::string(60, 'x');
    values["product"] = std::string(60, 'y');

    bool threw = false;
    try {
        codec.encode(values, image);
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

void testUnmodelledBytesSurvive() {
    const ftdi::Ft232rCodec codec;
    std::vector<uint8_t> image = blankImage(codec);
    image[0x0C] = 0x5A; // a byte no property owns
    image[0x00] = 0x91; // drive settings: only two bits belong to properties

    PropertyMap values = codec.decode(image);
    values["high_current_io"] = true;
    codec.encode(values, image);

    CHECK_EQUAL(int(image[0x0C]), 0x5A);
    CHECK_EQUAL(int(image[0x00] & ~0x06), 0x91 & ~0x06);
    CHECK_EQUAL(int(image[0x00] & 0x04), 0x04);
}

void testFt232rCbusPacking() {
    const ftdi::Ft232rCodec codec;
    std::vector<uint8_t> image = blankImage(codec);
    image[0x16] = 0xF0; // the upper nibble of 0x16 does not belong to CBUS4

    PropertyMap values = codec.decode(image);
    values["cbus0"] = uint32_t{0x0A}; // IOMODE
    values["cbus1"] = uint32_t{0x02}; // RXLED
    values["cbus2"] = uint32_t{0x03}; // TXLED
    values["cbus3"] = uint32_t{0x05}; // SLEEP
    values["cbus4"] = uint32_t{0x06}; // CLK48
    codec.encode(values, image);

    CHECK_EQUAL(int(image[0x14]), 0x2A); // cbus1 in the high nibble, cbus0 low
    CHECK_EQUAL(int(image[0x15]), 0x53);
    CHECK_EQUAL(int(image[0x16]), 0xF6); // upper nibble untouched

    const PropertyMap back = codec.decode(image);
    CHECK_EQUAL(getNumber(back, "cbus0"), 0x0Au);
    CHECK_EQUAL(getNumber(back, "cbus3"), 0x05u);
    CHECK_EQUAL(getNumber(back, "cbus4"), 0x06u);
}

void testFtxLayout() {
    const ftdi::FtxCodec codec;
    CHECK_EQUAL(codec.layout().size, std::size_t{256});

    std::vector<uint8_t> image = blankImage(codec);
    PropertyMap values = codec.decode(image);
    values["product"] = std::string("FT-X");
    values["cbus0"] = uint32_t{0x08}; // IOMODE
    values["cbus3"] = uint32_t{0x11}; // VBUS_SENSE
    codec.encode(values, image);

    CHECK(codec.verifyChecksum(image));
    CHECK_EQUAL(int(image[0x1A]), 0x08); // one full byte per CBUS pin
    CHECK_EQUAL(int(image[0x1D]), 0x11);
    // Strings live at 0xA0, where bit 7 of the offset byte is already set. The
    // manufacturer string is empty here, so it occupies only its two byte
    // header and the product string follows at 0xA2.
    CHECK_EQUAL(int(image[0x0E]), 0xA0);
    CHECK_EQUAL(int(image[0x10]), 0xA2);
    CHECK_EQUAL(getString(codec.decode(image), "product"), std::string("FT-X"));

    // The checksum skips the words between 0x12 and 0x40.
    const uint16_t before = codec.computeChecksum(image);
    image[0x30] ^= 0xFF;
    CHECK_EQUAL(codec.computeChecksum(image), before);
    image[0x82] ^= 0xFF;
    CHECK(codec.computeChecksum(image) != before);
}

void testFt4232hLayout() {
    const ftdi::Ft4232hCodec codec{256};
    CHECK_EQUAL(codec.layout().size, std::size_t{256});

    std::vector<uint8_t> image = blankImage(codec);
    PropertyMap values = codec.decode(image);
    values["product"] = std::string("Quad RS232-HS");
    codec.encode(values, image);

    CHECK(codec.verifyChecksum(image));
    // Strings start at 0x9A on the H series. The manufacturer string is empty
    // here, so it takes only its two byte header and the product follows.
    CHECK_EQUAL(int(image[0x0E]), 0x9A | 0x80);
    CHECK_EQUAL(int(image[0x10]), 0x9C | 0x80);
    CHECK_EQUAL(getString(codec.decode(image), "product"), std::string("Quad RS232-HS"));

    // Unlike the FT-X, the checksum covers one uninterrupted run of words, so
    // no byte below the checksum word may be ignored.
    const uint16_t before = codec.computeChecksum(image);
    image[0x30] ^= 0xFF;
    CHECK(codec.computeChecksum(image) != before);
}

/// An FT4232H with a 93C46 fitted. The chip wraps word addresses modulo the
/// part, so the string area at 0x9A folds onto 0x1A and the whole image has to
/// fit in 128 bytes. Writing a 256 byte image to such a board wraps its upper
/// half onto the header and destroys it, which is what this layout avoids.
void testFt4232hOn93c46() {
    const ftdi::Ft4232hCodec codec{128};
    CHECK_EQUAL(codec.layout().size, std::size_t{128});
    CHECK_EQUAL(codec.layout().checksumWord, 0x3F);

    std::vector<uint8_t> image = blankImage(codec);
    PropertyMap values = codec.decode(image);
    values["product"] = std::string("MotionAI");
    values["serial_number"] = std::string("MOTION-913");
    values["vcp_a"] = true;
    codec.encode(values, image);

    CHECK(codec.verifyChecksum(image));

    // The offset byte still records 0x9A, exactly as on a 256 byte part; only
    // the data moves, because readers mask the offset with the EEPROM size.
    CHECK_EQUAL(int(image[0x0E]), 0x9A);
    CHECK_EQUAL(int(image[0x1A]), 0x02);        // empty manufacturer, header only
    CHECK_EQUAL(int(image[0x1B]), 0x03);        // USB string descriptor type
    CHECK_EQUAL(int(image[0x1C]), 2 + (8 * 2)); // "MotionAI"

    const PropertyMap back = codec.decode(image);
    CHECK_EQUAL(getString(back, "product"), std::string("MotionAI"));
    CHECK_EQUAL(getString(back, "serial_number"), std::string("MOTION-913"));
    CHECK(getBool(back, "vcp_a"));

    // The strings must not reach the header, or they would land on the very
    // bytes that say where they are.
    CHECK(codec.layout().stringAreaBegin > 0x19);

    // A size the chip cannot have is refused rather than silently rounded.
    bool threw = false;
    try {
        const ftdi::Ft4232hCodec bad{512};
        (void)bad;
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

/// The FT4232H has no inverters, and 0x0B means something else entirely on it.
void testFt4232hHasNoInverters() {
    const ftdi::Ft4232hCodec codec{256};
    for (const PropertySpec& spec : codec.properties()) {
        CHECK(spec.name.rfind("invert_", 0) != 0);
    }
    // The other families keep theirs.
    const ftdi::Ft232rCodec ft232r;
    bool found = false;
    for (const PropertySpec& spec : ft232r.properties()) {
        found = found || spec.name == "invert_txd";
    }
    CHECK(found);
}

void testFt4232hPerChannelBits() {
    const ftdi::Ft4232hCodec codec{256};
    std::vector<uint8_t> image = blankImage(codec);
    PropertyMap values = codec.decode(image);

    // A blank image reads as the lowest drive strength, everything else off.
    CHECK_EQUAL(getNumber(values, "drive_a"), 4u);
    CHECK(!getBool(values, "vcp_a"));
    CHECK(!getBool(values, "rs485_d"));

    values["vcp_a"] = true; // low nibble of 0x00
    values["vcp_c"] = true; // high nibble of the same byte
    values["vcp_b"] = true; // low nibble of 0x01
    values["rs485_a"] = true;
    values["rs485_d"] = true;
    values["drive_a"] = uint32_t{16};
    values["drive_b"] = uint32_t{8};
    values["slow_slew_c"] = true;
    values["schmitt_d"] = true;
    codec.encode(values, image);

    CHECK_EQUAL(int(image[0x00]), 0x88); // vcp_a low, vcp_c high
    CHECK_EQUAL(int(image[0x01]), 0x08); // vcp_b low, vcp_d clear
    CHECK_EQUAL(int(image[0x0B]), 0x90); // rs485_a is 0x10, rs485_d is 0x80
    CHECK_EQUAL(int(image[0x0C]), 0x13); // channel A 16 mA, channel B 8 mA
    CHECK_EQUAL(int(image[0x0D]), 0x84); // channel C slow slew, channel D Schmitt

    const PropertyMap back = codec.decode(image);
    CHECK(getBool(back, "vcp_a"));
    CHECK(getBool(back, "vcp_b"));
    CHECK(getBool(back, "vcp_c"));
    CHECK(!getBool(back, "vcp_d"));
    CHECK(getBool(back, "rs485_a"));
    CHECK(!getBool(back, "rs485_b"));
    CHECK(getBool(back, "rs485_d"));
    CHECK_EQUAL(getNumber(back, "drive_a"), 16u);
    CHECK_EQUAL(getNumber(back, "drive_b"), 8u);
    CHECK_EQUAL(getNumber(back, "drive_c"), 4u);
    CHECK(getBool(back, "slow_slew_c"));
    CHECK(!getBool(back, "slow_slew_d"));
    CHECK(getBool(back, "schmitt_d"));
}

/// The RS485 byte shares its address with the FT232R inverter byte, so a stray
/// write from the common code would land on it. Guard that explicitly.
void testFt4232hKeepsUnmodelledRs485Bits() {
    const ftdi::Ft4232hCodec codec{256};
    std::vector<uint8_t> image = blankImage(codec);
    image[0x0B] = 0x0F; // the reserved low nibble
    image[0x18] = 0x66; // the EEPROM type marker, not modelled either

    PropertyMap values = codec.decode(image);
    values["rs485_b"] = true;
    codec.encode(values, image);

    CHECK_EQUAL(int(image[0x0B]), 0x2F); // rs485_b set, low nibble intact
    CHECK_EQUAL(int(image[0x18]), 0x66);
}

/// A factory image read from a real FT232R with 'usbprog read'. It is the only
/// reference here that did not come out of this code, so it is what proves the
/// checksum algorithm and the layout right rather than merely self-consistent.
/// Replace it with a dump of your own chip if you prefer.
const std::vector<uint8_t>& factoryFt232rImage() {
    // clang-format off
    static const std::vector<uint8_t> kImage = {
        0x00, 0x40, 0x03, 0x04, 0x01, 0x60, 0x00, 0x00, 0xa0, 0x2d, 0x08, 0x00,
        0x00, 0x00, 0x98, 0x0a, 0xa2, 0x20, 0xc2, 0x12, 0x23, 0x10, 0x05, 0x00,
        0x0a, 0x03, 0x46, 0x00, 0x54, 0x00, 0x44, 0x00, 0x49, 0x00, 0x20, 0x03,
        0x46, 0x00, 0x54, 0x00, 0x32, 0x00, 0x33, 0x00, 0x32, 0x00, 0x52, 0x00,
        0x20, 0x00, 0x55, 0x00, 0x53, 0x00, 0x42, 0x00, 0x20, 0x00, 0x55, 0x00,
        0x41, 0x00, 0x52, 0x00, 0x54, 0x00, 0x12, 0x03, 0x42, 0x00, 0x30, 0x00,
        0x30, 0x00, 0x31, 0x00, 0x41, 0x00, 0x4d, 0x00, 0x4a, 0x00, 0x48, 0x00,
        0x9d, 0x31, 0x21, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xb1,
    };
    // clang-format on
    return kImage;
}

void testFactoryImageChecksum() {
    const ftdi::Ft232rCodec codec;
    const std::vector<uint8_t>& image = factoryFt232rImage();
    CHECK_EQUAL(image.size(), std::size_t{128});
    CHECK(codec.verifyChecksum(image));
    CHECK_EQUAL(codec.computeChecksum(image), 0xB106);
}

void testFactoryImageDecodesToTheDatasheetDefaults() {
    const ftdi::Ft232rCodec codec;
    const PropertyMap values = codec.decode(factoryFt232rImage());

    CHECK_EQUAL(getNumber(values, "vendor_id"), 0x0403u);
    CHECK_EQUAL(getNumber(values, "product_id"), 0x6001u);
    CHECK_EQUAL(getString(values, "manufacturer"), std::string("FTDI"));
    CHECK_EQUAL(getString(values, "product"), std::string("FT232R USB UART"));
    CHECK_EQUAL(getNumber(values, "max_power"), 90u);
    CHECK(getBool(values, "use_serial"));
    CHECK(getBool(values, "remote_wakeup"));
    CHECK(!getBool(values, "self_powered"));

    // The CBUS defaults the FT232R datasheet lists for a factory fresh chip.
    CHECK_EQUAL(getNumber(values, "cbus0"), 0x03u); // TXLED
    CHECK_EQUAL(getNumber(values, "cbus1"), 0x02u); // RXLED
    CHECK_EQUAL(getNumber(values, "cbus2"), 0x00u); // TXDEN
    CHECK_EQUAL(getNumber(values, "cbus3"), 0x01u); // PWREN
    CHECK_EQUAL(getNumber(values, "cbus4"), 0x05u); // SLEEP
}

void testEncodingAFactoryImageChangesNothing() {
    const ftdi::Ft232rCodec codec;
    std::vector<uint8_t> image = factoryFt232rImage();
    const PropertyMap values = codec.decode(image);
    codec.encode(values, image);

    // Writing back what we read must reproduce the image byte for byte,
    // including the four factory bytes that sit behind the strings at 0x54.
    CHECK(image == factoryFt232rImage());
}

void testChangingOneSettingLeavesTheRestAlone() {
    const ftdi::Ft232rCodec codec;
    std::vector<uint8_t> image = factoryFt232rImage();
    PropertyMap values = codec.decode(image);
    values["cbus4"] = uint32_t{0x08}; // CLK12
    codec.encode(values, image);

    CHECK(codec.verifyChecksum(image));
    const PropertyMap back = codec.decode(image);
    CHECK_EQUAL(getNumber(back, "cbus4"), 0x08u);
    CHECK_EQUAL(getString(back, "product"), std::string("FT232R USB UART"));
    CHECK_EQUAL(getString(back, "serial_number"), std::string("B001AMJH"));

    // Only the CBUS byte and the checksum word may move. (One of the two
    // checksum bytes can happen to keep its old value, so the count is not
    // fixed.)
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < image.size(); ++i) {
        if (image[i] != factoryFt232rImage()[i]) {
            changed.push_back(i);
        }
    }
    CHECK(!changed.empty());
    CHECK_EQUAL(changed.front(), std::size_t{0x16});
    for (const std::size_t index : changed) {
        CHECK(index == 0x16 || index >= 0x7E);
    }
}

/// A blank EEPROM decodes its identity as 0xffff. Because `set` is a
/// read-modify-write, naming only the strings on a fresh chip would otherwise
/// commit that 0xffff with a valid checksum and strand the device as
/// ffff:ffff, findable only with --driver. The programmer fills the blanks
/// from what the device reports over USB instead.
void testBlankIdentityIsSeededFromTheDescriptor() {
    const ftdi::Ft4232hCodec codec{128};
    const std::vector<uint8_t> blank(codec.layout().size, 0xFF);

    // A blank EEPROM decodes to the placeholder, which is what makes a partial
    // `set` dangerous in the first place.
    PropertyMap values = codec.decode(blank);
    CHECK_EQUAL(getNumber(values, "vendor_id"), 0xFFFFu);
    CHECK_EQUAL(getNumber(values, "product_id"), 0xFFFFu);

    // A chip running on its built-in defaults still reports the real IDs.
    const std::vector<std::string> filled = codec.seedIdentity(values, 0x0403, 0x6011);
    CHECK_EQUAL(filled.size(), std::size_t{2});
    CHECK_EQUAL(getNumber(values, "vendor_id"), 0x0403u);
    CHECK_EQUAL(getNumber(values, "product_id"), 0x6011u);

    // Encoding the seeded map yields an image that probes correctly, which is
    // the whole point: the same map without seeding would enumerate ffff:ffff.
    std::vector<uint8_t> image = blank;
    codec.encode(values, image);
    CHECK(codec.verifyChecksum(image));
    CHECK_EQUAL(getNumber(codec.decode(image), "vendor_id"), 0x0403u);

    // Values that already hold something real are never overwritten, so
    // seeding cannot undo a deliberately reprogrammed vendor ID.
    PropertyMap custom = codec.decode(image);
    custom["vendor_id"] = uint32_t{0x1234};
    CHECK(codec.seedIdentity(custom, 0x0403, 0x6011).empty());
    CHECK_EQUAL(getNumber(custom, "vendor_id"), 0x1234u);

    // Once a device has already been stranded as ffff:ffff the descriptor
    // repeats the bad value, so there is nothing to copy and the write guard
    // has to be the one that catches it.
    PropertyMap stranded = codec.decode(blank);
    CHECK(codec.seedIdentity(stranded, 0xFFFF, 0xFFFF).empty());
    CHECK_EQUAL(getNumber(stranded, "vendor_id"), 0xFFFFu);

    // Seeding is per property: a blank product_id is filled even when the
    // vendor_id is already good.
    PropertyMap half = codec.decode(blank);
    half["vendor_id"] = uint32_t{0x0403};
    const std::vector<std::string> one = codec.seedIdentity(half, 0x0403, 0x6011);
    CHECK_EQUAL(one.size(), std::size_t{1});
    CHECK_EQUAL(one.front(), std::string("product_id"));
}

/// The probe logic decides which driver touches which chip, so it is worth a
/// test of its own; DeviceInfo is a plain struct, no hardware needed.
void testDriverRegistry() {
    const Registry& registry = Registry::builtin();
    CHECK(registry.drivers().size() >= 2);
    CHECK(registry.byId("ft232r") != nullptr);
    CHECK(registry.byId("FT232R") != nullptr); // lookup is case insensitive
    CHECK(registry.byId("no-such-driver") == nullptr);

    // FTDI puts the chip generation in bcdDevice.
    usb::DeviceInfo info;
    info.vendorId = 0x0403;
    info.bcdDevice = 0x0600;
    const Driver* driver = registry.probe(info);
    CHECK(driver != nullptr);
    if (driver != nullptr) {
        CHECK_EQUAL(driver->id(), std::string("ft232r"));
        CHECK(!driver->properties().empty()); // schema without a device attached
        CHECK(!driver->description().empty());
    }

    info.bcdDevice = 0x1000;
    driver = registry.probe(info);
    CHECK(driver != nullptr);
    if (driver != nullptr) {
        CHECK_EQUAL(driver->id(), std::string("ftx"));
    }

    // Another vendor that happens to report the same release number is not an
    // FTDI chip; reaching it needs an explicit --driver.
    info.vendorId = 0x1234;
    info.bcdDevice = 0x0600;
    CHECK(registry.probe(info) == nullptr);

    info.vendorId = 0x0403;
    info.bcdDevice = 0x0800;
    driver = registry.probe(info);
    CHECK(driver != nullptr);
    if (driver != nullptr) {
        CHECK_EQUAL(driver->id(), std::string("ft4232h"));
    }

    info.bcdDevice = 0x0900; // FT232H, no driver for it yet
    CHECK(registry.probe(info) == nullptr);
    info.bcdDevice = 0x0700; // FT2232H, same
    CHECK(registry.probe(info) == nullptr);
}

void testPropertyParsing() {
    PropertySpec boolean;
    boolean.name = "flag";
    boolean.type = PropertyType::Bool;
    CHECK(std::get<bool>(parse(boolean, "on")));
    CHECK(std::get<bool>(parse(boolean, "YES")));
    CHECK(!std::get<bool>(parse(boolean, "0")));

    PropertySpec number;
    number.name = "max_power";
    number.type = PropertyType::Number;
    number.maximum = 500;
    number.step = 2;
    CHECK_EQUAL(std::get<uint32_t>(parse(number, "0x64")), 100u);
    bool threw = false;
    try {
        parse(number, "101"); // not a multiple of the step
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        parse(number, "502"); // above the maximum
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);

    PropertySpec choice;
    choice.name = "cbus0";
    choice.type = PropertyType::Enum;
    choice.values = {{"TXDEN", 0, ""}, {"IOMODE", 10, ""}};
    CHECK_EQUAL(std::get<uint32_t>(parse(choice, "iomode")), 10u);
    CHECK_EQUAL(format(choice, PropertyValue{uint32_t{10}}), std::string("IOMODE"));
}

void testTextHelpers() {
    const std::vector<uint16_t> units = text::toUtf16("aä€");
    CHECK_EQUAL(units.size(), std::size_t{3});
    CHECK_EQUAL(text::toUtf8(units), std::string("aä€"));

    // Characters outside the basic plane need a surrogate pair.
    CHECK_EQUAL(text::utf16Length("🙂"), std::size_t{2});
    CHECK_EQUAL(text::toUtf8(text::toUtf16("🙂")), std::string("🙂"));

    uint16_t vendorId = 0;
    uint16_t productId = 0;
    text::parseVidPid("0403:6001", vendorId, productId);
    CHECK_EQUAL(vendorId, 0x0403);
    CHECK_EQUAL(productId, 0x6001);
}

} // namespace

int main() {
    // A test that throws is a failure, not a crash.
    try {
        testChecksumOfZeroImage();
        testChecksumIsStoredInTheLastWord();
        testIdentifiersAndPower();
        testStringRoundTrip();
        testStringsThatDoNotFitAreRejected();
        testUnmodelledBytesSurvive();
        testFt232rCbusPacking();
        testFtxLayout();
        testFt4232hLayout();
        testFt4232hOn93c46();
        testFt4232hHasNoInverters();
        testFt4232hPerChannelBits();
        testFt4232hKeepsUnmodelledRs485Bits();
        testFactoryImageChecksum();
        testFactoryImageDecodesToTheDatasheetDefaults();
        testEncodingAFactoryImageChangesNothing();
        testChangingOneSettingLeavesTheRestAlone();
        testBlankIdentityIsSeededFromTheDescriptor();
        testDriverRegistry();
        testPropertyParsing();
        testTextHelpers();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
        return 1;
    }

    std::cout << checks - failures << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
