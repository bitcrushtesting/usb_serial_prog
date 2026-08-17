# usb_serial_prog

[![build & test](https://img.shields.io/github/actions/workflow/status/bitcrushtesting/usb_serial_prog/ci.yml?branch=main&label=build%20%26%20test)](https://github.com/bitcrushtesting/usb_serial_prog/actions/workflows/ci.yml)
[![coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/bitcrushtesting/usb_serial_prog/main/.github/badges/coverage.json)](https://github.com/bitcrushtesting/usb_serial_prog/actions/workflows/ci.yml)
[![licence: GPL v2](https://img.shields.io/badge/licence-GPLv2-blue.svg)](LICENSE)

`usbprog` reads and rewrites the configuration EEPROM of USB serial chips: USB
identifiers, the manufacturer/product/serial strings, bus power settings, signal
inversion and the function of the CBUS pins.

It does the same job as [`ft232r_prog`](https://github.com/kiwitea/ft232r_prog),
in modern C++ and with a driver layer, so that support for chips other than the
FT232R is a matter of adding one class rather than rewriting the tool. It talks
to `libusb-1.0` directly and needs neither `libftdi` nor FTDI's proprietary
`D2XX` library.

## Supported chips

| Driver    | Chips | EEPROM | Status |
| --------- | ----- | ------ | ------ |
| `ft232r`  | FT232R, FT245R | 128 bytes, internal | tested against hardware |
| `ftx`     | FT230X, FT231X, FT234XD | 256 bytes | experimental, see below |
| `ft4232h` | FT4232H | 256 bytes, external | experimental, see below |

The FT-X and FT4232H layouts follow the same public description as the FT232R
one, but have had far less exposure to real silicon. `usbprog` will not write to
a chip whose current EEPROM contents fail the checksum this tool computes, which
is a direct test of whether it understands the layout — see [Safety](#safety).

The FT4232H has four independent UARTs and, unlike the other two families, no
CBUS pins and no signal inverters. Its settings therefore come in per-channel
sets suffixed `_a` to `_d` — `vcp_*`, `rs485_*`, `drive_*`, `schmitt_*` and
`slow_slew_*`. Run `usbprog props` against a connected chip for the full list.

### FT4232H EEPROM size

The FT4232H's EEPROM is external rather than on-die, and which part is fitted
changes the layout. The chip wraps EEPROM word addresses modulo the size of the
part, so on a 93C46 the string area at 0x9A folds down onto 0x1A:

| Part            | Size      | Strings at | Checksum word |
| --------------- | --------- | ---------- | ------------- |
| 93C46           | 128 bytes | 0x1A       | 0x3F          |
| 93C56, 93C66    | 256 bytes | 0x9A       | 0x7F          |

This matters because the wrapping is not harmless: writing a 256 byte image to
a 93C46 folds the upper half back onto the header and destroys it. `usbprog`
therefore measures the part before writing — a smaller EEPROM read through a
larger window shows up as its contents mirrored — and refuses to write an image
that would wrap.

A blank EEPROM cannot be measured, because it reads the same at every size. In
that one case you have to say which part is fitted:

```sh
usbprog set --eeprom-size 128 vendor_id=0x0403 product_id=0x6011 product="..."
```

Getting it wrong is safe as long as the chip is blank: a mismatch between
`--eeprom-size` and a later measurement is reported rather than written.

### Blank chips and the USB identity

`set` is a read-modify-write: it decodes what is on the chip, applies the
settings you name, and writes the result back. On a chip with a blank EEPROM
every field you do *not* name decodes as `0xff`, so naming only the strings
would commit `vendor_id`/`product_id` as `0xffff` — together with a freshly
computed, and therefore *valid*, checksum. The chip then believes it and
enumerates as `ffff:ffff`, where no probe can find it.

This mostly matters for external EEPROMs, which arrive blank. The FT232R never
showed it, because its internal EEPROM ships pre-programmed.

Two things prevent it. A blank identity is seeded from what the device reports
over USB, which is correct because a chip with a blank EEPROM runs on its
built-in defaults:

```
$ usbprog set product="MotionAI"
note: vendor_id read as 0xffff (a blank EEPROM); using the value the device
      reports over USB instead
```

Anything you name yourself is applied afterwards and wins, so a deliberately
reprogrammed vendor ID is never undone. And once a device has already been
stranded the descriptor repeats the bad value, so there is nothing to seed
from; writing `0xffff` is refused outright instead:

```
$ usbprog write -i stranded.bin
usbprog: vendor_id is 0xffff, which is what a blank EEPROM reads rather than a
real USB ID. [...] repeat with --force if you truly want it.
```

To recover a device already enumerating as `ffff:ffff`, reach it explicitly and
name the right IDs:

```sh
usbprog set --device ffff:ffff --driver ft4232h vendor_id=0x0403 product_id=0x6011
```

## Installing a release

Each tagged release carries archives for Linux (x86_64, arm64) and macOS
(arm64) on the [releases page](https://github.com/bitcrushtesting/usb_serial_prog/releases).
They link libusb statically, so they only need the C library and, on Linux,
`libudev`:

```sh
tar -xzf usbprog-<version>-linux-x86_64.tar.gz
cd usbprog-<version>-linux-x86_64
sudo install -m 0755 usbprog /usr/local/bin/
sudo install -m 0644 60-usbprog.rules /etc/udev/rules.d/   # Linux only
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Otherwise build it yourself.

## Building

Requirements: a C++20 compiler (GCC 10+, Clang 12+, Apple Clang 13+), CMake 3.16
or newer, and `libusb-1.0`.

### Ubuntu / Debian

```sh
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build           # /usr/local/bin/usbprog plus the udev rule
```

### macOS

```sh
brew install cmake libusb
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

CMake finds `libusb-1.0` through `pkg-config` when available and falls back to
searching the usual prefixes, so a Homebrew install without `pkg-config` works
too.

### Tests

The unit tests cover the EEPROM encoders and need no hardware:

```sh
ctest --test-dir build --output-on-failure
```

They include a factory image dumped from a real FT232R, which is what pins down
the checksum algorithm and the byte layout.

For a coverage report:

```sh
cmake -S . -B coverage-build -DCMAKE_BUILD_TYPE=Debug -DUSBPROG_COVERAGE=ON
cmake --build coverage-build
ctest --test-dir coverage-build
gcovr --root . --filter src/core/ --filter src/drivers/ \
      --exclude 'src/core/usb\.(cpp|h)' --exclude 'src/drivers/ftdi/ftdi_driver\.cpp' \
      --txt - coverage-build
```

The badge measures that scope: everything the unit tests can reach. The libusb
transport needs a real device and the CLI is covered by the smoke test in CI, so
neither is counted; the EEPROM codecs, where a mistake ends up on somebody's
chip, sit above 95%.

## Permissions

### Linux

Out of the box only root may talk to a USB device directly. Install the bundled
rule (`cmake --install` does this for you) and reload udev:

```sh
sudo cp packaging/60-usbprog.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Then replug the device. The kernel's `ftdi_sio` driver holds the interface while
the device is bound to it; `usbprog` asks libusb to detach it for the duration of
the command and the kernel reattaches it afterwards, so `/dev/ttyUSB0` comes back
by itself.

### macOS

No setup is needed. If macOS's own FTDI driver holds the interface, `usbprog`
prints a warning and continues: EEPROM access runs over endpoint 0, which stays
available. Close any serial terminal that has the port open first.

## Usage

```
usbprog [options] <command> [arguments]
```

| Command | What it does |
| ------- | ------------ |
| `list` | connected devices and the driver that claims each one |
| `drivers` | chip families this build supports |
| `props` | settings the selected device understands |
| `info` | current configuration, decoded |
| `dump` | EEPROM contents as a hex dump |
| `read -o FILE` | save the raw EEPROM image |
| `write -i FILE` | write a raw EEPROM image back |
| `set KEY=VALUE ...` | change individual settings |
| `erase` | erase the EEPROM, reverting to the chip's factory defaults |

Run `usbprog --help` for the full option list.

### Looking around

```console
$ usbprog list
bus:addr  vid:pid    driver    device
002:005   0403:6001  ft232r    FTDI FT232R USB UART  [B001AMJH]

$ usbprog info
Device    002:005 (0403:6001)
Chip      FT232R [driver ft232r, 128 byte EEPROM]
Checksum  ok

  vendor_id            0x0403
  product_id           0x6001
  manufacturer         FTDI
  product              FT232R USB UART
  serial_number        B001AMJH
  use_serial           on
  self_powered         off
  remote_wakeup        on
  max_power            90 mA
  ...
  cbus0                TXLED
  cbus1                RXLED
  cbus2                TXDEN
  cbus3                PWREN
  cbus4                SLEEP
```

### Changing settings

`set` takes any number of `key=value` pairs, applies them together and shows what
would change. `-n` stops before writing:

```console
$ usbprog set -n product="Widget Bridge" cbus4=CLK12 max_power=200
Device    002:005 (0403:6001)
Chip      FT232R [driver ft232r, 128 byte EEPROM]

Changes:
  product    FT232R USB UART  ->  Widget Bridge
  max_power  90 mA  ->  200 mA
  cbus4      SLEEP  ->  CLK12

30 of 128 bytes differ.
Dry run: nothing was written.
```

Drop the `-n` and confirm to write. Values are typed: `on`/`off` (also
`yes`/`no`, `1`/`0`) for switches, decimal or `0x` hex for numbers, and names
from `usbprog props` for the CBUS pins. Strings are UTF-8 and are stored as
UTF-16 USB string descriptors.

The three strings share the same EEPROM area — 48 characters in total on the
FT232R — and `usbprog` refuses the change rather than truncating.

### Backup and restore

```sh
usbprog read -o ft232r-backup.bin      # before you change anything
usbprog write -i ft232r-backup.bin     # put it back
```

`set`, `write` and `erase` save the current contents to
`usbprog-backup-<vid>-<pid>-<timestamp>.bin` first, unless you pass `--backup
FILE` or `--no-backup`.

### Picking a device

With more than one chip connected, `usbprog` lists the candidates and stops.
Narrow it down with `--device 0403:6001`, `--serial B001AMJH`, `--bus`/`--address`
or `--index N`.

After you change a chip's vendor ID, probing no longer recognises it. Select it
explicitly instead:

```sh
usbprog --device 1234:5678 --driver ft232r info
```

## Safety

Reprogramming an EEPROM is not risk-free, so the tool works to a few rules:

- **Nothing is written without confirmation.** Every writing command asks, unless
  you pass `--yes`. When stdin is not a terminal, a missing `--yes` is an error
  rather than a hang.
- **A backup is written first**, unless you opt out.
- **Only known bytes are touched.** `set` decodes the image, applies your
  changes, and writes the result back into the *existing* image. Bytes this tool
  does not model — including the factory data an FT232R keeps behind the
  strings — are preserved. Re-encoding an untouched factory image reproduces it
  byte for byte; there is a test for exactly that.
- **The checksum is the correctness gate.** Before modifying anything, `usbprog`
  recomputes the checksum of what is already on the chip and compares it with
  the stored one. A mismatch means the tool's model of the layout is wrong (or
  the chip is blank), and it refuses to write. This is why the experimental FT-X
  support cannot quietly corrupt a chip it does not understand.
- **Every write is read back and verified**, byte for byte.
- **Recovery**: an FTDI chip with an invalid checksum falls back to its factory
  defaults (`0403:6001` for an FT232R) instead of disappearing, so a bad image
  is recoverable. Write your backup, or `usbprog erase` and start over.

`--force` overrides the checksum gate when you know what you are doing.

Exit status is `0` on success, `1` when you decline a confirmation, `2` for a
reported error, and `3` for an unexpected one.

## Adding support for another chip

The interfaces are in `src/core/driver.h`. A chip family needs two things: a
`Driver` that recognises the device, and a `Programmer` that moves bytes and
translates them into named properties. Everything the CLI prints, parses and
validates is derived from the property list, so no command needs to know about
your chip.

1. **Describe the settings.** Build a `std::vector<PropertySpec>`, one entry per
   setting, with a type (`Bool`, `Number`, `String`, `Enum`), a help line, and
   for enums the value names. `usbprog props`, argument parsing and value
   formatting all follow from this.

2. **Implement `Programmer`.**

   ```cpp
   class MyProgrammer final : public usbprog::Programmer {
       std::string chipName() const override { return "CH340"; }
       std::size_t eepromSize() const override { return 64; }

       std::vector<uint8_t> readEeprom() override;            // however the chip does it
       void writeEeprom(std::span<const uint8_t>) override;
       void eraseEeprom() override;

       const std::vector<PropertySpec>& properties() const override;
       PropertyMap decode(std::span<const uint8_t>) const override;
       void encode(const PropertyMap&, std::vector<uint8_t>&) const override;
       bool verifyChecksum(std::span<const uint8_t>) const override;
   };
   ```

   `encode` must patch the image it is given rather than build a fresh one, so
   that unknown bytes survive, and it owns recomputing whatever integrity field
   the chip uses. `verifyChecksum` is what lets the CLI refuse to write to a chip
   it does not understand — implement it honestly, or return `false` and let
   users pass `--force`.

3. **Implement `Driver`** with `probe()` (match on vendor/product ID or whatever
   identifies the family), `attach()` and the offline property list, then add one
   line to `registerBuiltinDrivers()` in `src/core/registry.cpp`.

For a worked example, `src/drivers/ftdi/` splits this up: `ftdi_driver.cpp` holds
the USB transport shared by all FTDI chips, `ftdi_codec.*` the EEPROM fields they
have in common, and `ft232r.cpp` / `ftx.cpp` only the per-chip differences —
about 100 lines each.

## Layout

```
src/core/       USB access (libusb RAII wrappers), property model, driver
                interfaces, driver registry
src/drivers/    chip support, one directory per vendor
src/cli/        argument parsing, commands, output formatting
tests/          EEPROM encoder tests, no hardware needed
packaging/      udev rule for Linux
```

`usbprog_core` is a static library holding everything except the CLI, so other
programs can link against the same driver layer.

## Development

The repository carries its own conventions, and CI enforces all three:

- **`.clang-format`** — 100 columns, four space indent. Reformat with
  `find src tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i`.
- **`.clang-tidy`** — bugprone, modernize, performance and readability checks,
  with the noisy ones switched off and the naming scheme spelled out. Run it
  against a configured build directory:
  `clang-tidy -p build $(find src tests -name '*.cpp')`. The tree is clean, so
  CI treats any finding as an error.
- **`.editorconfig`** — line endings, final newlines and indent width, for
  editors that do not read the clang-format file.

CI also builds an instrumented binary, checks that unit test coverage has not
dropped below its floor, publishes the HTML report as a build artifact, and
refreshes `.github/badges/coverage.json` on `main`, which is what the coverage
badge reads. (Both badges need the repository to be public to render.)

CI pins the LLVM tools to a known version (see `.github/workflows/ci.yml`), so
a new LLVM release cannot fail the build overnight. Bump the pin and reformat in
the same commit.

Every source file starts with an SPDX header; CI fails if one is missing.

### Cutting a release

Push a version tag and the release workflow builds, tests and attaches the
archives to the GitHub release for that tag:

```sh
git tag -a v0.2.0 -m 'usbprog 0.2.0'
git push origin v0.2.0
```

The tag is passed to CMake as `USBPROG_VERSION_OVERRIDE`, so `usbprog --version`
always matches the release. Release builds also set `-DUSBPROG_STATIC_LIBUSB=ON`,
which links libusb into the binary; you can use that flag for local builds too.

## Licence

GPL-2.0-only. Copyright (C) 2026 Bitcrush Testing. See [LICENSE](LICENSE) for
the full text, and the SPDX header in each source file.

## Credits

The FT232R EEPROM layout is the one documented by the
[libftdi](https://www.intra2net.com/en/developer/libftdi/) project and FTDI's
own application notes; `ft232r_prog` by Mark Lord was the model for what a tool
like this should be able to do.
