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

#include "drivers/ch34x/ch340_config.h"

#include "core/error.h"
#include "core/text.h"

#include <algorithm>

namespace usbprog::ch34x {
namespace {

uint16_t read16(std::span<const uint8_t> image, uint16_t offset) {
    return static_cast<uint16_t>(image[offset] | (image[offset + 1] << 8));
}

void write16(std::vector<uint8_t>& image, uint16_t offset, uint16_t value) {
    image[offset] = static_cast<uint8_t>(value & 0xFF);
    image[offset + 1] = static_cast<uint8_t>(value >> 8);
}

/// The datasheet's test for a usable serial number: the first byte has to be a
/// printable ASCII character, otherwise the chip reports no serial at all.
bool isPrintableAscii(uint8_t byte) { return byte >= 0x21 && byte <= 0x7E; }

/// The serial number is a fixed eight byte field rather than a descriptor, so
/// a shorter value is padded and the padding has to be dropped again here.
std::string readSerial(std::span<const uint8_t> image) {
    if (!isPrintableAscii(image[addr::kSerialNumber])) {
        return {};
    }
    std::string serial;
    for (std::size_t i = 0; i < config::kSerialChars; ++i) {
        const uint8_t byte = image[addr::kSerialNumber + i];
        if (!isPrintableAscii(byte)) {
            break;
        }
        serial.push_back(static_cast<char>(byte));
    }
    return serial;
}

std::string readProduct(std::span<const uint8_t> image) {
    const std::size_t bytes = image[addr::kProduct];
    // A length of zero is the documented "use the vendor default" marker, and
    // anything that does not describe a string descriptor inside the area is
    // treated the same way by the chip.
    if (bytes < 4 || bytes > config::kProductDescriptorBytes || (bytes % 2) != 0) {
        return {};
    }
    if (image[addr::kProduct + 1] != 0x03) {
        return {};
    }
    std::vector<uint16_t> units;
    units.reserve((bytes - 2) / 2);
    for (std::size_t i = addr::kProduct + 2; i + 1 < addr::kProduct + bytes; i += 2) {
        units.push_back(static_cast<uint16_t>(image[i] | (image[i + 1] << 8)));
    }
    return text::toUtf8(units);
}

void writeSerial(std::vector<uint8_t>& image, const std::string& serial) {
    if (serial.size() > config::kSerialChars) {
        throw Error("serial_number: the CH340B stores it as " +
                    std::to_string(config::kSerialChars) + " ASCII characters, and '" + serial +
                    "' is " + std::to_string(serial.size()) + " characters long");
    }
    for (const char character : serial) {
        if (!isPrintableAscii(static_cast<uint8_t>(character))) {
            throw Error("serial_number: the CH340B stores it as plain ASCII, so '" + serial +
                        "' cannot be represented. Use printable ASCII (0x21-0x7e) only.");
        }
    }
    // Padding with zero both fills the field and marks where the value ends.
    for (std::size_t i = 0; i < config::kSerialChars; ++i) {
        image[addr::kSerialNumber + i] =
            i < serial.size() ? static_cast<uint8_t>(serial[i]) : uint8_t{0x00};
    }
}

void writeProduct(std::vector<uint8_t>& image, const std::string& product) {
    const std::vector<uint16_t> units = text::toUtf16(product);
    if (units.size() > config::kProductChars) {
        throw Error("product: the CH340B has room for " + std::to_string(config::kProductChars) +
                    " characters of product string, and '" + product + "' needs " +
                    std::to_string(units.size()));
    }

    // Clear the whole field first: unlike the FTDI string heap nothing else
    // lives here, and leaving the tail of a longer previous string behind
    // would be visible in a dump.
    std::fill(image.begin() + addr::kProduct, image.begin() + config::kBytes, uint8_t{0x00});
    if (units.empty()) {
        return; // length byte 0x00: the chip uses its own description
    }

    const auto bytes = static_cast<uint8_t>(2 + (units.size() * 2));
    image[addr::kProduct] = bytes;
    image[addr::kProduct + 1] = 0x03; // USB string descriptor type
    for (std::size_t i = 0; i < units.size(); ++i) {
        image[addr::kProduct + 2 + (i * 2)] = static_cast<uint8_t>(units[i] & 0xFF);
        image[addr::kProduct + 3 + (i * 2)] = static_cast<uint8_t>(units[i] >> 8);
    }
}

} // namespace

Ch340Config::Ch340Config() {
    PropertySpec vendorId;
    vendorId.name = "vendor_id";
    vendorId.type = PropertyType::Number;
    vendorId.hexadecimal = true;
    vendorId.maximum = 0xFFFF;
    vendorId.help = "USB vendor ID (0x0000 or 0xffff means the WCH default 1a86:7523)";
    addProperty(std::move(vendorId));

    PropertySpec productId;
    productId.name = "product_id";
    productId.type = PropertyType::Number;
    productId.hexadecimal = true;
    productId.maximum = 0xFFFF;
    productId.help = "USB product ID";
    addProperty(std::move(productId));

    PropertySpec product;
    product.name = "product";
    product.type = PropertyType::String;
    product.maxChars = config::kProductChars;
    product.help = "product string descriptor (empty for the chip's own description)";
    addProperty(std::move(product));

    PropertySpec serial;
    serial.name = "serial_number";
    serial.type = PropertyType::String;
    serial.maxChars = config::kSerialChars;
    serial.help = "serial number, up to 8 printable ASCII characters";
    addProperty(std::move(serial));

    PropertySpec useSerial;
    useSerial.name = "use_serial";
    useSerial.type = PropertyType::Bool;
    useSerial.help = "report the serial number to the host";
    addProperty(std::move(useSerial));

    PropertySpec maxPower;
    maxPower.name = "max_power";
    maxPower.type = PropertyType::Number;
    maxPower.unit = "mA";
    maxPower.maximum = 500;
    maxPower.step = 2;
    maxPower.help = "current drawn from the bus (0-500 mA, in steps of 2)";
    addProperty(std::move(maxPower));

    // Both of the following describe the state of the memory rather than a
    // setting, and neither can be changed by writing an image: the driver
    // always activates the area it writes, and it never writes the lock byte.
    PropertySpec active;
    active.name = "config_active";
    active.type = PropertyType::Bool;
    active.writable = false;
    active.help = "chip is reading this area (false: it runs on the vendor defaults)";
    addProperty(std::move(active));

    PropertySpec writeProtect;
    writeProtect.name = "write_protected";
    writeProtect.type = PropertyType::Bool;
    writeProtect.writable = false;
    writeProtect.help = "area has been locked against further writes";
    addProperty(std::move(writeProtect));
}

void Ch340Config::addProperty(PropertySpec spec) { properties_.push_back(std::move(spec)); }

void Ch340Config::requireSize(std::size_t size) {
    if (size != config::kBytes) {
        throw Error("CH340B configuration image has " + std::to_string(size) + " bytes, expected " +
                    std::to_string(config::kBytes));
    }
}

bool Ch340Config::isActive(std::span<const uint8_t> image) {
    return image[addr::kSig] == config::kSigInternal;
}

bool Ch340Config::isWriteProtected(std::span<const uint8_t> image) {
    return image[addr::kWriteProtect] == config::kWriteProtected;
}

PropertyMap Ch340Config::decode(std::span<const uint8_t> image) const {
    requireSize(image.size());

    PropertyMap values;
    values["config_active"] = isActive(image);
    values["write_protected"] = isWriteProtected(image);

    if (!isActive(image)) {
        // The chip is not reading these bytes, so reporting them would
        // describe something that has no effect on the device in front of the
        // user. Report what the chip is actually doing instead.
        values["vendor_id"] = static_cast<uint32_t>(config::kDefaultVendorId);
        values["product_id"] = static_cast<uint32_t>(config::kDefaultProductId);
        values["product"] = std::string();
        values["serial_number"] = std::string();
        values["use_serial"] = (config::kDefaultConfig & config::kSerialDisabled) == 0;
        values["max_power"] = static_cast<uint32_t>(config::kDefaultMaxPowerUnits) * 2;
        return values;
    }

    values["vendor_id"] = static_cast<uint32_t>(read16(image, addr::kVendorId));
    values["product_id"] = static_cast<uint32_t>(read16(image, addr::kProductId));
    values["product"] = readProduct(image);
    values["serial_number"] = readSerial(image);
    values["use_serial"] = (image[addr::kConfig] & config::kSerialDisabled) == 0;
    values["max_power"] = static_cast<uint32_t>(image[addr::kMaxPower]) * 2;
    return values;
}

void Ch340Config::encode(const PropertyMap& values, std::vector<uint8_t>& image) const {
    requireSize(image.size());

    // Whether the chip was using this area decides where the bits nobody has
    // named come from: its own contents when it was, the datasheet defaults
    // when it was not. Read it before the signature below overwrites it.
    const bool wasActive = isActive(image);

    // Writing settings the chip is not reading is never what anyone means, so
    // any encode activates the area.
    image[addr::kSig] = config::kSigInternal;
    image[addr::kMode] = config::kModeSerial;

    uint8_t configByte = wasActive ? image[addr::kConfig] : config::kDefaultConfig;
    configByte = static_cast<uint8_t>(getBool(values, "use_serial")
                                          ? (configByte & ~config::kSerialDisabled)
                                          : (configByte | config::kSerialDisabled));
    image[addr::kConfig] = configByte;

    write16(image, addr::kVendorId, static_cast<uint16_t>(getNumber(values, "vendor_id")));
    write16(image, addr::kProductId, static_cast<uint16_t>(getNumber(values, "product_id")));

    // Bus power is stored in 2 mA units; parsing already enforced the step.
    image[addr::kMaxPower] = static_cast<uint8_t>(getNumber(values, "max_power") / 2);

    writeSerial(image, getString(values, "serial_number"));
    writeProduct(image, getString(values, "product"));

    // addr::kWriteProtect is deliberately left alone. Writing 0x57 there locks
    // the area for good, and no property maps onto it for that reason.
}

bool Ch340Config::verifyChecksum(std::span<const uint8_t> image) const {
    requireSize(image.size());
    // There is no checksum. An inactive area is fine whatever it holds, since
    // the chip ignores it; an active one has to carry the serial mode, because
    // the datasheet defines no other and we would not know what we are
    // looking at.
    return !isActive(image) || image[addr::kMode] == config::kModeSerial;
}

} // namespace usbprog::ch34x
