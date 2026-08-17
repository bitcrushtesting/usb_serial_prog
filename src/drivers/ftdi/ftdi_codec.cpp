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

#include "drivers/ftdi/ftdi_codec.h"

#include "core/error.h"
#include "core/text.h"

namespace usbprog::ftdi {
namespace {

uint16_t read16(std::span<const uint8_t> image, uint16_t offset) {
    return static_cast<uint16_t>(image[offset] | (image[offset + 1] << 8));
}

void write16(std::vector<uint8_t>& image, uint16_t offset, uint16_t value) {
    image[offset] = static_cast<uint8_t>(value & 0xFF);
    image[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void setBit(uint8_t& byte, uint8_t mask, bool set) {
    byte = static_cast<uint8_t>(set ? (byte | mask) : (byte & ~mask));
}

PropertySpec boolProperty(std::string name, std::string help) {
    PropertySpec spec;
    spec.name = std::move(name);
    spec.type = PropertyType::Bool;
    spec.help = std::move(help);
    return spec;
}

struct InvertBit {
    const char* name;
    uint8_t mask;
    const char* signalHelp;
};

constexpr InvertBit kInvertBits[] = {
    {"invert_txd", invert::kTxd, "TXD"},  {"invert_rxd", invert::kRxd, "RXD"},
    {"invert_rts", invert::kRts, "RTS#"}, {"invert_cts", invert::kCts, "CTS#"},
    {"invert_dtr", invert::kDtr, "DTR#"}, {"invert_dsr", invert::kDsr, "DSR#"},
    {"invert_dcd", invert::kDcd, "DCD#"}, {"invert_ri", invert::kRi, "RI#"},
};

} // namespace

Codec::Codec(Layout layout) : layout_(std::move(layout)) { addCommonProperties(); }

void Codec::addProperty(PropertySpec spec) { properties_.push_back(std::move(spec)); }

uint32_t Codec::nibble(uint8_t byte, bool high) {
    return high ? static_cast<uint32_t>(byte >> 4) : static_cast<uint32_t>(byte & 0x0F);
}

void Codec::setNibble(uint8_t& byte, bool high, uint32_t value) {
    const auto nibbleValue = static_cast<uint8_t>(value & 0x0F);
    byte = high ? static_cast<uint8_t>((byte & 0x0F) | (nibbleValue << 4))
                : static_cast<uint8_t>((byte & 0xF0) | nibbleValue);
}

PropertySpec Codec::cbusProperty(const std::string& name, std::vector<EnumValue> values,
                                 const std::string& help) {
    PropertySpec spec;
    spec.name = name;
    spec.type = PropertyType::Enum;
    spec.help = help;
    spec.values = std::move(values);
    return spec;
}

std::size_t Codec::stringBudget() const {
    // Three strings, each with a two byte descriptor header, two bytes per
    // UTF-16 code unit.
    const std::size_t bytes = layout_.stringAreaEnd - layout_.stringAreaBegin;
    return bytes < 6 ? 0 : (bytes - 6) / 2;
}

void Codec::addCommonProperties() {
    PropertySpec vendorId;
    vendorId.name = "vendor_id";
    vendorId.type = PropertyType::Number;
    vendorId.hexadecimal = true;
    vendorId.maximum = 0xFFFF;
    vendorId.help = "USB vendor ID reported by the device";
    addProperty(std::move(vendorId));

    PropertySpec productId;
    productId.name = "product_id";
    productId.type = PropertyType::Number;
    productId.hexadecimal = true;
    productId.maximum = 0xFFFF;
    productId.help = "USB product ID reported by the device";
    addProperty(std::move(productId));

    const std::size_t budget = stringBudget();
    PropertySpec manufacturer;
    manufacturer.name = "manufacturer";
    manufacturer.type = PropertyType::String;
    manufacturer.maxChars = budget;
    manufacturer.help = "manufacturer string descriptor";
    addProperty(std::move(manufacturer));

    PropertySpec product;
    product.name = "product";
    product.type = PropertyType::String;
    product.maxChars = budget;
    product.help = "product string descriptor";
    addProperty(std::move(product));

    PropertySpec serial;
    serial.name = "serial_number";
    serial.type = PropertyType::String;
    serial.maxChars = budget;
    serial.help = "serial number string descriptor";
    addProperty(std::move(serial));

    addProperty(boolProperty("use_serial", "report the serial number to the host"));
    addProperty(boolProperty("self_powered", "device is powered from its own supply"));
    addProperty(boolProperty("remote_wakeup", "device may wake the host from suspend"));

    PropertySpec maxPower;
    maxPower.name = "max_power";
    maxPower.type = PropertyType::Number;
    maxPower.unit = "mA";
    maxPower.maximum = 500;
    maxPower.step = 2;
    maxPower.help = "current drawn from the bus (0-500 mA, in steps of 2)";
    addProperty(std::move(maxPower));

    addProperty(boolProperty("suspend_pull_downs", "pull down the I/O pins while suspended"));

    if (layout_.signalInversion) {
        for (const auto& bit : kInvertBits) {
            addProperty(
                boolProperty(bit.name, std::string("invert the ") + bit.signalHelp + " signal"));
        }
    }
}

std::vector<std::string> Codec::seedIdentity(PropertyMap& values, uint16_t reportedVendorId,
                                             uint16_t reportedProductId) const {
    struct Seed {
        const char* name;
        uint16_t reported;
    };
    const Seed seeds[] = {
        {"vendor_id", reportedVendorId},
        {"product_id", reportedProductId},
    };

    std::vector<std::string> filled;
    for (const Seed& seed : seeds) {
        const auto it = values.find(seed.name);
        if (it == values.end() || getNumber(values, seed.name) != kBlankId) {
            continue; // holds a real value already
        }
        if (seed.reported == kBlankId || seed.reported == 0) {
            continue; // nothing usable to copy from
        }
        it->second = static_cast<uint32_t>(seed.reported);
        filled.emplace_back(seed.name);
    }
    return filled;
}

void Codec::requireSize(std::size_t size) const {
    if (size != layout_.size) {
        throw Error("EEPROM image has " + std::to_string(size) + " bytes, expected " +
                    std::to_string(layout_.size));
    }
}

std::string Codec::readString(std::span<const uint8_t> image, uint16_t offsetAddr,
                              uint16_t lengthAddr) const {
    // The offset byte has bit 7 set as a marker; mask it off with the EEPROM
    // size so it also works for chips whose string area lives above 0x80.
    const std::size_t offset = image[offsetAddr] & (layout_.size - 1);
    const std::size_t length = image[lengthAddr];
    if (length < 4 || offset + length > layout_.size) {
        return {}; // blank or corrupt: nothing sensible to show
    }
    if (image[offset + 1] != 0x03) {
        return {}; // not a string descriptor
    }
    std::vector<uint16_t> units;
    units.reserve((length - 2) / 2);
    for (std::size_t i = offset + 2; i + 1 < offset + length; i += 2) {
        units.push_back(static_cast<uint16_t>(image[i] | (image[i + 1] << 8)));
    }
    return text::toUtf8(units);
}

void Codec::writeStrings(std::vector<uint8_t>& image, const std::string& manufacturer,
                         const std::string& product, const std::string& serial) const {
    struct Entry {
        const std::string& value;
        uint16_t offsetAddr;
        uint16_t lengthAddr;
        const char* name;
    };
    const Entry entries[] = {
        {manufacturer, addr::kManufacturerOffset, addr::kManufacturerLength, "manufacturer"},
        {product, addr::kProductOffset, addr::kProductLength, "product"},
        {serial, addr::kSerialOffset, addr::kSerialLength, "serial_number"},
    };

    // Only the bytes the descriptors actually occupy are rewritten. Factory
    // images keep data behind the strings (a real FT232R has four such bytes),
    // and zero-filling the whole area would destroy it.
    std::size_t position = layout_.stringAreaBegin;

    for (const auto& entry : entries) {
        const std::vector<uint16_t> units = text::toUtf16(entry.value);
        const std::size_t bytes = 2 + (units.size() * 2);
        if (bytes > 0xFF || position + bytes > layout_.stringAreaEnd) {
            throw Error("the strings do not fit into the EEPROM: manufacturer, product and "
                        "serial_number share " +
                        std::to_string(stringBudget()) + " characters, and '" + entry.name +
                        "' overflows that budget");
        }
        image[position] = static_cast<uint8_t>(bytes);
        image[position + 1] = 0x03; // USB string descriptor type
        for (std::size_t i = 0; i < units.size(); ++i) {
            image[position + 2 + (i * 2)] = static_cast<uint8_t>(units[i] & 0xFF);
            image[position + 3 + (i * 2)] = static_cast<uint8_t>(units[i] >> 8);
        }
        image[entry.offsetAddr] = static_cast<uint8_t>(position | 0x80);
        image[entry.lengthAddr] = static_cast<uint8_t>(bytes);
        position += bytes;
    }
}

PropertyMap Codec::decode(std::span<const uint8_t> image) const {
    requireSize(image.size());

    PropertyMap values;
    values["vendor_id"] = static_cast<uint32_t>(read16(image, addr::kVendorId));
    values["product_id"] = static_cast<uint32_t>(read16(image, addr::kProductId));
    values["manufacturer"] =
        readString(image, addr::kManufacturerOffset, addr::kManufacturerLength);
    values["product"] = readString(image, addr::kProductOffset, addr::kProductLength);
    values["serial_number"] = readString(image, addr::kSerialOffset, addr::kSerialLength);

    const uint8_t configByte = image[addr::kConfigDescriptor];
    values["self_powered"] = (configByte & config::kSelfPowered) != 0;
    values["remote_wakeup"] = (configByte & config::kRemoteWakeup) != 0;
    values["max_power"] = static_cast<uint32_t>(image[addr::kMaxPower]) * 2;

    const uint8_t chipByte = image[addr::kChipConfig];
    values["use_serial"] = (chipByte & chip::kUseSerial) != 0;
    values["suspend_pull_downs"] = (chipByte & chip::kSuspendPullDowns) != 0;

    if (layout_.signalInversion) {
        const uint8_t invertByte = image[addr::kInvert];
        for (const auto& bit : kInvertBits) {
            values[bit.name] = (invertByte & bit.mask) != 0;
        }
    }

    decodeChip(image, values);
    return values;
}

void Codec::encode(const PropertyMap& values, std::vector<uint8_t>& image) const {
    requireSize(image.size());

    write16(image, addr::kVendorId, static_cast<uint16_t>(getNumber(values, "vendor_id")));
    write16(image, addr::kProductId, static_cast<uint16_t>(getNumber(values, "product_id")));

    uint8_t configByte = image[addr::kConfigDescriptor];
    configByte |= config::kReserved;
    setBit(configByte, config::kSelfPowered, getBool(values, "self_powered"));
    setBit(configByte, config::kRemoteWakeup, getBool(values, "remote_wakeup"));
    image[addr::kConfigDescriptor] = configByte;

    // Bus power is stored in 2 mA units; parsing already enforced the step.
    image[addr::kMaxPower] = static_cast<uint8_t>(getNumber(values, "max_power") / 2);

    uint8_t chipByte = image[addr::kChipConfig];
    setBit(chipByte, chip::kUseSerial, getBool(values, "use_serial"));
    setBit(chipByte, chip::kSuspendPullDowns, getBool(values, "suspend_pull_downs"));
    image[addr::kChipConfig] = chipByte;

    if (layout_.signalInversion) {
        uint8_t invertByte = 0;
        for (const auto& bit : kInvertBits) {
            setBit(invertByte, bit.mask, getBool(values, bit.name));
        }
        image[addr::kInvert] = invertByte;
    }

    writeStrings(image, getString(values, "manufacturer"), getString(values, "product"),
                 getString(values, "serial_number"));

    encodeChip(values, image);

    const uint16_t checksum = computeChecksum(image);
    write16(image, static_cast<uint16_t>(layout_.checksumWord * 2), checksum);
}

uint16_t Codec::computeChecksum(std::span<const uint8_t> image) const {
    requireSize(image.size());
    uint16_t checksum = 0xAAAA;
    for (const WordRange& range : layout_.checksumWords) {
        for (uint16_t word = range.begin; word < range.end; ++word) {
            const uint16_t value = read16(image, static_cast<uint16_t>(word * 2));
            checksum = static_cast<uint16_t>(checksum ^ value);
            checksum = static_cast<uint16_t>((checksum << 1) | (checksum >> 15));
        }
    }
    return checksum;
}

uint16_t Codec::storedChecksum(std::span<const uint8_t> image) const {
    requireSize(image.size());
    return read16(image, static_cast<uint16_t>(layout_.checksumWord * 2));
}

bool Codec::verifyChecksum(std::span<const uint8_t> image) const {
    return computeChecksum(image) == storedChecksum(image);
}

} // namespace usbprog::ftdi
