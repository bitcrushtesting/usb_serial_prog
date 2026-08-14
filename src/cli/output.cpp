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

#include "cli/output.h"

#include "core/error.h"
#include "core/text.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
#include <io.h>
#define USBPROG_ISATTY _isatty
#define USBPROG_FILENO _fileno
#else
#include <unistd.h>
#define USBPROG_ISATTY isatty
#define USBPROG_FILENO fileno
#endif

namespace usbprog::cli {

void hexdump(std::ostream& out, std::span<const uint8_t> data, std::size_t baseAddress) {
    constexpr std::size_t kPerLine = 16;
    for (std::size_t offset = 0; offset < data.size(); offset += kPerLine) {
        char address[16];
        std::snprintf(address, sizeof(address), "%04zx", baseAddress + offset);
        out << address << "  ";

        const std::size_t count = std::min(kPerLine, data.size() - offset);
        for (std::size_t i = 0; i < kPerLine; ++i) {
            if (i == kPerLine / 2) {
                out << ' ';
            }
            if (i < count) {
                out << text::hex8(data[offset + i]) << ' ';
            } else {
                out << "   ";
            }
        }

        out << " |";
        for (std::size_t i = 0; i < count; ++i) {
            const uint8_t byte = data[offset + i];
            out << static_cast<char>(byte >= 0x20 && byte < 0x7F ? byte : '.');
        }
        out << "|\n";
    }
}

std::string padRight(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

void printTable(std::ostream& out, const std::vector<std::pair<std::string, std::string>>& rows,
                const std::string& indent) {
    std::size_t width = 0;
    for (const auto& row : rows) {
        width = std::max(width, row.first.size());
    }
    for (const auto& row : rows) {
        out << indent << padRight(row.first, width) << "  " << row.second << "\n";
    }
}

bool confirm(const std::string& question, bool assumeYes) {
    if (assumeYes) {
        return true;
    }
    if (USBPROG_ISATTY(USBPROG_FILENO(stdin)) == 0) {
        throw Error("refusing to continue without confirmation: stdin is not a terminal, so pass "
                    "--yes if you mean it");
    }
    std::cout << question << " [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    answer = text::trim(answer);
    return text::iequals(answer, "y") || text::iequals(answer, "yes");
}

std::vector<uint8_t> readBinaryFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw Error("cannot open '" + path + "' for reading");
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
    if (file.bad()) {
        throw Error("reading '" + path + "' failed");
    }
    return data;
}

void writeBinaryFile(const std::string& path, std::span<const uint8_t> data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw Error("cannot open '" + path + "' for writing");
    }
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.close();
    if (!file) {
        throw Error("writing '" + path + "' failed");
    }
}

std::string defaultBackupName(uint16_t vendorId, uint16_t productId) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    return "usbprog-backup-" + text::hex16(vendorId) + "-" + text::hex16(productId) + "-" + stamp +
           ".bin";
}

} // namespace usbprog::cli
