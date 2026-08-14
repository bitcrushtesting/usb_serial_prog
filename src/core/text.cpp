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

#include "core/text.h"

#include "core/error.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

namespace usbprog::text {
namespace {

void appendCodePoint(std::vector<uint16_t>& out, uint32_t cp) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<uint16_t>(cp));
    } else {
        cp -= 0x10000;
        out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
    }
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

std::vector<uint16_t> toUtf16(std::string_view utf8) {
    std::vector<uint16_t> out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::size_t extra = 0;
        uint32_t cp = 0;
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1Fu;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0Fu;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            cp = lead & 0x07u;
            extra = 3;
        } else {
            throw Error("invalid UTF-8 input");
        }
        if (i + extra >= utf8.size()) {
            throw Error("truncated UTF-8 input");
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(utf8[i + k]);
            if ((cont & 0xC0) != 0x80) {
                throw Error("invalid UTF-8 continuation byte");
            }
            cp = (cp << 6) | (cont & 0x3Fu);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            throw Error("invalid Unicode code point in input");
        }
        appendCodePoint(out, cp);
        i += extra + 1;
    }
    return out;
}

std::string toUtf8(std::span<const uint16_t> utf16) {
    std::string out;
    out.reserve(utf16.size());
    for (std::size_t i = 0; i < utf16.size(); ++i) {
        uint32_t cp = utf16[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 &&
            utf16[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (utf16[++i] - 0xDC00);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD; // lone surrogate: keep going, this is untrusted EEPROM data
        }
        appendUtf8(out, cp);
    }
    return out;
}

std::size_t utf16Length(std::string_view utf8) { return toUtf16(utf8).size(); }

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

std::string trim(std::string_view s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    const auto* begin = std::find_if(s.begin(), s.end(), notSpace);
    const auto* end = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::optional<uint32_t> tryParseUnsigned(std::string_view s, uint32_t maximum) {
    const std::string text = trim(s);
    if (text.empty() || text[0] == '-' || text[0] == '+') {
        return std::nullopt;
    }
    int base = 10;
    std::size_t offset = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        offset = 2;
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        offset = 2;
    }
    unsigned long long value = 0;
    std::size_t consumed = 0;
    try {
        value = std::stoull(text.substr(offset), &consumed, base);
    } catch (const std::exception&) {
        return std::nullopt; // not a number at all
    }
    if (consumed != text.size() - offset || value > maximum) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

uint32_t parseUnsigned(std::string_view s, uint32_t maximum) {
    const std::string text = trim(s);
    const std::optional<uint32_t> value =
        tryParseUnsigned(text, std::numeric_limits<uint32_t>::max());
    if (!value) {
        throw Error("'" + text + "' is not a valid number");
    }
    if (*value > maximum) {
        throw Error("value " + text + " exceeds the maximum of " + std::to_string(maximum));
    }
    return *value;
}

void parseVidPid(std::string_view s, uint16_t& vid, uint16_t& pid) {
    const auto colon = s.find(':');
    if (colon == std::string_view::npos) {
        throw Error("expected VID:PID (for example 0403:6001), got '" + std::string(s) + "'");
    }
    vid = static_cast<uint16_t>(parseUnsigned("0x" + std::string(s.substr(0, colon)), 0xFFFF));
    pid = static_cast<uint16_t>(parseUnsigned("0x" + std::string(s.substr(colon + 1)), 0xFFFF));
}

std::string hex16(uint16_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04x", value);
    return buffer;
}

std::string hex8(uint8_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02x", value);
    return buffer;
}

} // namespace usbprog::text
