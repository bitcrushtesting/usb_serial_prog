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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace usbprog::usb {

/// Everything we know about a device before a driver looks at it.
struct DeviceInfo {
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint16_t bcdDevice = 0; ///< FTDI encodes the chip generation here
    uint8_t busNumber = 0;
    uint8_t deviceAddress = 0;
    std::string manufacturer;
    std::string product;
    std::string serial;
    bool descriptorsReadable = false; ///< false when the strings could not be read

    std::string address() const; ///< "003:017"
};

/// Which USB device the user meant. Every set field must match.
struct DeviceFilter {
    std::optional<uint16_t> vendorId;
    std::optional<uint16_t> productId;
    std::optional<std::string> serial;
    std::optional<uint8_t> busNumber;
    std::optional<uint8_t> deviceAddress;
    std::optional<std::size_t> index; ///< pick the n-th match (0 based)

    bool matches(const DeviceInfo& info) const;
    bool empty() const;
    std::string describe() const;
};

/// An open device. Move-only; closes on destruction.
class Handle {
public:
    Handle() = default;
    explicit Handle(libusb_device_handle* handle) : handle_(handle) {}
    ~Handle();
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    explicit operator bool() const noexcept { return handle_ != nullptr; }
    libusb_device_handle* raw() const noexcept { return handle_; }

    /// Ask the kernel to release the interface while we hold it (Linux). Returns
    /// false when the platform or the kernel driver does not cooperate; callers
    /// decide whether that is fatal.
    bool detachKernelDriver(int interfaceNumber);
    bool claimInterface(int interfaceNumber);
    void releaseInterface();

    /// Vendor control transfers, the channel FTDI uses for EEPROM access. The
    /// parameters carry the names the USB specification gives the fields of the
    /// setup packet.
    void controlIn(uint8_t bRequest, uint16_t wValue, uint16_t wIndex, std::span<uint8_t> data,
                   unsigned timeoutMs = 2000);
    void controlOut(uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                    std::span<const uint8_t> data = {}, unsigned timeoutMs = 2000);

    std::string stringDescriptor(uint8_t index) const;

private:
    libusb_device_handle* handle_ = nullptr;
    std::optional<int> claimed_;
};

/// A device found by enumeration. Holds a libusb reference.
class Device {
public:
    Device() = default;
    explicit Device(libusb_device* device);
    ~Device();
    Device(const Device& other);
    Device& operator=(const Device& other);
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    /// Descriptor data. String descriptors need the device open, so this opens
    /// it briefly; when that is not permitted the string fields stay empty and
    /// descriptorsReadable is false.
    DeviceInfo info() const;

    Handle open() const;

private:
    libusb_device* device_ = nullptr;
};

/// libusb library context. One per process is enough.
class Context {
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    std::vector<Device> devices() const;

    libusb_context* raw() const noexcept { return context_; }

private:
    libusb_context* context_ = nullptr;
};

/// Human readable libusb error, including the hint about permissions that
/// nearly always explains LIBUSB_ERROR_ACCESS.
std::string errorText(int code);

} // namespace usbprog::usb
