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

#include "core/usb.h"

#include "core/error.h"
#include "core/text.h"

#include <libusb.h>

#include <cstdio>
#include <utility>

namespace usbprog::usb {
namespace {

// The casts keep the compiler from complaining about combining values of
// different libusb enumerations, which is exactly what bmRequestType is.
constexpr uint8_t kRequestTypeVendorIn = static_cast<uint8_t>(LIBUSB_ENDPOINT_IN) |
                                         static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR) |
                                         static_cast<uint8_t>(LIBUSB_RECIPIENT_DEVICE);
constexpr uint8_t kRequestTypeVendorOut = static_cast<uint8_t>(LIBUSB_ENDPOINT_OUT) |
                                          static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR) |
                                          static_cast<uint8_t>(LIBUSB_RECIPIENT_DEVICE);

[[noreturn]] void fail(const std::string& what, int code) {
    throw UsbError(what + ": " + errorText(code), code);
}

} // namespace

std::string errorText(int code) {
    std::string text = libusb_error_name(code);
    if (const char* description = libusb_strerror(static_cast<libusb_error>(code))) {
        text += " (";
        text += description;
        text += ")";
    }
    if (code == LIBUSB_ERROR_ACCESS) {
        text += " - insufficient permissions; see the udev rules section of the README";
    }
    return text;
}

std::string DeviceInfo::address() const {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%03u:%03u", static_cast<unsigned>(busNumber),
                  static_cast<unsigned>(deviceAddress));
    return buffer;
}

bool DeviceFilter::empty() const {
    return !vendorId && !productId && !serial && !busNumber && !deviceAddress && !index;
}

bool DeviceFilter::matches(const DeviceInfo& info) const {
    if (vendorId && *vendorId != info.vendorId) {
        return false;
    }
    if (productId && *productId != info.productId) {
        return false;
    }
    if (busNumber && *busNumber != info.busNumber) {
        return false;
    }
    if (deviceAddress && *deviceAddress != info.deviceAddress) {
        return false;
    }
    if (serial && *serial != info.serial) {
        return false;
    }
    return true;
}

std::string DeviceFilter::describe() const {
    std::string out;
    const auto add = [&out](const std::string& part) { out += (out.empty() ? "" : ", ") + part; };
    if (vendorId || productId) {
        add("id " + (vendorId ? text::hex16(*vendorId) : std::string("*")) + ":" +
            (productId ? text::hex16(*productId) : std::string("*")));
    }
    if (serial) {
        add("serial '" + *serial + "'");
    }
    if (busNumber) {
        add("bus " + std::to_string(*busNumber));
    }
    if (deviceAddress) {
        add("address " + std::to_string(*deviceAddress));
    }
    if (index) {
        add("index " + std::to_string(*index));
    }
    return out.empty() ? "any supported device" : out;
}

Handle::~Handle() {
    releaseInterface();
    if (handle_ != nullptr) {
        libusb_close(handle_);
    }
}

Handle::Handle(Handle&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      claimed_(std::exchange(other.claimed_, std::nullopt)) {}

Handle& Handle::operator=(Handle&& other) noexcept {
    if (this != &other) {
        releaseInterface();
        if (handle_ != nullptr) {
            libusb_close(handle_);
        }
        handle_ = std::exchange(other.handle_, nullptr);
        claimed_ = std::exchange(other.claimed_, std::nullopt);
    }
    return *this;
}

bool Handle::detachKernelDriver(int interfaceNumber) {
    if (handle_ == nullptr) {
        return false;
    }
    // Preferred path: libusb reattaches the kernel driver when we close.
    if (libusb_set_auto_detach_kernel_driver(handle_, 1) == LIBUSB_SUCCESS) {
        return true;
    }
    if (libusb_kernel_driver_active(handle_, interfaceNumber) == 1) {
        return libusb_detach_kernel_driver(handle_, interfaceNumber) == LIBUSB_SUCCESS;
    }
    return true;
}

bool Handle::claimInterface(int interfaceNumber) {
    if (handle_ == nullptr) {
        return false;
    }
    if (libusb_claim_interface(handle_, interfaceNumber) != LIBUSB_SUCCESS) {
        return false;
    }
    claimed_ = interfaceNumber;
    return true;
}

void Handle::releaseInterface() {
    if (handle_ != nullptr && claimed_) {
        libusb_release_interface(handle_, *claimed_);
        claimed_.reset();
    }
}

void Handle::controlIn(uint8_t bRequest, uint16_t wValue, uint16_t wIndex, std::span<uint8_t> data,
                       unsigned timeoutMs) {
    if (handle_ == nullptr) {
        throw Error("internal error: control transfer on a closed device");
    }
    const int rc =
        libusb_control_transfer(handle_, kRequestTypeVendorIn, bRequest, wValue, wIndex,
                                data.data(), static_cast<uint16_t>(data.size()), timeoutMs);
    if (rc < 0) {
        fail("control read (request 0x" + text::hex8(bRequest) + ", index " +
                 std::to_string(wIndex) + ") failed",
             rc);
    }
    if (static_cast<std::size_t>(rc) != data.size()) {
        throw Error("short control read: expected " + std::to_string(data.size()) + " bytes, got " +
                    std::to_string(rc));
    }
}

void Handle::controlOut(uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                        std::span<const uint8_t> data, unsigned timeoutMs) {
    if (handle_ == nullptr) {
        throw Error("internal error: control transfer on a closed device");
    }
    const int rc = libusb_control_transfer(handle_, kRequestTypeVendorOut, bRequest, wValue, wIndex,
                                           const_cast<unsigned char*>(data.data()),
                                           static_cast<uint16_t>(data.size()), timeoutMs);
    if (rc < 0) {
        fail("control write (request 0x" + text::hex8(bRequest) + ", index " +
                 std::to_string(wIndex) + ") failed",
             rc);
    }
}

std::string Handle::stringDescriptor(uint8_t index) const {
    if (handle_ == nullptr || index == 0) {
        return {};
    }
    unsigned char buffer[256] = {};
    const int rc = libusb_get_string_descriptor_ascii(handle_, index, buffer, sizeof(buffer));
    if (rc < 0) {
        return {};
    }
    return {reinterpret_cast<char*>(buffer), static_cast<std::size_t>(rc)};
}

Device::Device(libusb_device* device) : device_(device) {
    if (device_ != nullptr) {
        libusb_ref_device(device_);
    }
}

Device::~Device() {
    if (device_ != nullptr) {
        libusb_unref_device(device_);
    }
}

Device::Device(const Device& other) : Device(other.device_) {}

Device& Device::operator=(const Device& other) {
    if (this != &other) {
        if (other.device_ != nullptr) {
            libusb_ref_device(other.device_);
        }
        if (device_ != nullptr) {
            libusb_unref_device(device_);
        }
        device_ = other.device_;
    }
    return *this;
}

Device::Device(Device&& other) noexcept : device_(std::exchange(other.device_, nullptr)) {}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (device_ != nullptr) {
            libusb_unref_device(device_);
        }
        device_ = std::exchange(other.device_, nullptr);
    }
    return *this;
}

DeviceInfo Device::info() const {
    if (device_ == nullptr) {
        throw Error("internal error: querying an empty device");
    }
    libusb_device_descriptor descriptor{};
    const int rc = libusb_get_device_descriptor(device_, &descriptor);
    if (rc != LIBUSB_SUCCESS) {
        fail("reading the device descriptor failed", rc);
    }

    DeviceInfo info;
    info.vendorId = descriptor.idVendor;
    info.productId = descriptor.idProduct;
    info.bcdDevice = descriptor.bcdDevice;
    info.busNumber = libusb_get_bus_number(device_);
    info.deviceAddress = libusb_get_device_address(device_);

    // String descriptors require an open device. Not being allowed to open it is
    // normal (other users' devices, missing udev rule), so this stays quiet.
    libusb_device_handle* raw = nullptr;
    if (libusb_open(device_, &raw) == LIBUSB_SUCCESS) {
        const Handle handle(raw);
        info.manufacturer = handle.stringDescriptor(descriptor.iManufacturer);
        info.product = handle.stringDescriptor(descriptor.iProduct);
        info.serial = handle.stringDescriptor(descriptor.iSerialNumber);
        info.descriptorsReadable = true;
    }
    return info;
}

Handle Device::open() const {
    if (device_ == nullptr) {
        throw Error("internal error: opening an empty device");
    }
    libusb_device_handle* raw = nullptr;
    const int rc = libusb_open(device_, &raw);
    if (rc != LIBUSB_SUCCESS) {
        fail("opening the device failed", rc);
    }
    return Handle(raw);
}

Context::Context() {
    const int rc = libusb_init(&context_);
    if (rc != LIBUSB_SUCCESS) {
        fail("libusb initialisation failed", rc);
    }
}

Context::~Context() {
    if (context_ != nullptr) {
        libusb_exit(context_);
    }
}

std::vector<Device> Context::devices() const {
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context_, &list);
    if (count < 0) {
        fail("enumerating USB devices failed", static_cast<int>(count));
    }
    std::vector<Device> devices;
    devices.reserve(static_cast<std::size_t>(count));
    for (ssize_t i = 0; i < count; ++i) {
        devices.emplace_back(list[i]); // Device takes its own reference
    }
    libusb_free_device_list(list, 1);
    return devices;
}

} // namespace usbprog::usb
