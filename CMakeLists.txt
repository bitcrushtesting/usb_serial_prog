# SPDX-License-Identifier: GPL-2.0-only
#
# usb_serial_prog - configuration EEPROM programmer for USB serial chips
# Copyright (C) 2026 Bitcrush Testing

cmake_minimum_required(VERSION 3.16)

project(usb_serial_prog
  VERSION 0.1.0
  DESCRIPTION "Command line programmer for the configuration EEPROM of USB serial chips"
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Handy for clangd and other tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(USBPROG_BUILD_TESTS "Build the unit tests" ON)
option(USBPROG_WERROR "Treat compiler warnings as errors" OFF)
option(USBPROG_STATIC_LIBUSB "Link libusb-1.0 statically, for redistributable binaries" OFF)
option(USBPROG_COVERAGE "Instrument the code for coverage measurement" OFF)

# Release builds are tagged, and the tag is the authority on the version. Local
# builds fall back to the project() version.
set(USBPROG_VERSION_OVERRIDE "" CACHE STRING "Version string to report instead of the project one")
if(USBPROG_VERSION_OVERRIDE)
  set(USBPROG_VERSION "${USBPROG_VERSION_OVERRIDE}")
else()
  set(USBPROG_VERSION "${PROJECT_VERSION}")
endif()

find_package(LibUSB1 REQUIRED)

# ---------------------------------------------------------------------------
# Core library: USB access, the driver interface and the chip drivers.
# The CLI links against it, and so do the tests.
# ---------------------------------------------------------------------------
add_library(usbprog_core STATIC
  src/core/property.cpp
  src/core/registry.cpp
  src/core/text.cpp
  src/core/usb.cpp
  src/drivers/ftdi/ft232r.cpp
  src/drivers/ftdi/ft4232h.cpp
  src/drivers/ftdi/ftdi_codec.cpp
  src/drivers/ftdi/ftdi_driver.cpp
  src/drivers/ftdi/ftx.cpp
)

target_include_directories(usbprog_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(usbprog_core PUBLIC LibUSB1::LibUSB1)
target_compile_definitions(usbprog_core PUBLIC USBPROG_VERSION="${USBPROG_VERSION}")

if(MSVC)
  target_compile_options(usbprog_core PRIVATE /W4)
else()
  target_compile_options(usbprog_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()
if(USBPROG_COVERAGE)
  if(MSVC)
    message(FATAL_ERROR "USBPROG_COVERAGE is not supported with MSVC")
  endif()
  # gcov-compatible counters; gcovr reads them from both GCC and Clang.
  target_compile_options(usbprog_core PUBLIC --coverage -O0 -g)
  target_link_options(usbprog_core PUBLIC --coverage)
endif()

if(USBPROG_WERROR)
  if(MSVC)
    target_compile_options(usbprog_core PRIVATE /WX)
  else()
    target_compile_options(usbprog_core PRIVATE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# The command line tool.
# ---------------------------------------------------------------------------
add_executable(usbprog
  src/main.cpp
  src/cli/commands.cpp
  src/cli/options.cpp
  src/cli/output.cpp
)
target_link_libraries(usbprog PRIVATE usbprog_core)
if(NOT MSVC)
  target_compile_options(usbprog PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# ---------------------------------------------------------------------------
# Tests: EEPROM encoding and decoding, no hardware required.
# ---------------------------------------------------------------------------
if(USBPROG_BUILD_TESTS)
  enable_testing()
  add_executable(usbprog_tests tests/test_eeprom.cpp)
  target_link_libraries(usbprog_tests PRIVATE usbprog_core)
  add_test(NAME eeprom COMMAND usbprog_tests)
endif()

# ---------------------------------------------------------------------------
# Installation.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS usbprog RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES packaging/60-usbprog.rules
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/udev/rules.d
  )
endif()

message(STATUS "usb_serial_prog ${USBPROG_VERSION}")
message(STATUS "  libusb-1.0: ${LibUSB1_LIBRARY} (headers: ${LibUSB1_INCLUDE_DIR})")
message(STATUS "  static:     ${USBPROG_STATIC_LIBUSB}")
message(STATUS "  tests:      ${USBPROG_BUILD_TESTS}")
