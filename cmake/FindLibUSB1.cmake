# SPDX-License-Identifier: GPL-2.0-only
#
# usb_serial_prog - configuration EEPROM programmer for USB serial chips
# Copyright (C) 2026 Bitcrush Testing

# Locate libusb-1.0 and expose it as the imported target LibUSB1::LibUSB1.
#
# pkg-config is used when available (the normal case on Linux); otherwise the
# usual install prefixes are searched, which covers Homebrew and MacPorts on
# macOS where pkg-config is often missing.
#
# Result variables:
#   LibUSB1_FOUND, LibUSB1_INCLUDE_DIRS, LibUSB1_LIBRARIES, LibUSB1_VERSION

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_LIBUSB1 QUIET libusb-1.0)
endif()

find_path(LibUSB1_INCLUDE_DIR
  NAMES libusb.h
  HINTS ${PC_LIBUSB1_INCLUDEDIR} ${PC_LIBUSB1_INCLUDE_DIRS}
  PATHS
    /opt/homebrew/include
    /usr/local/include
    /opt/local/include
    /usr/include
  PATH_SUFFIXES libusb-1.0
)

# A static libusb makes the release archives usable on machines that do not
# have the shared library installed.
if(USBPROG_STATIC_LIBUSB)
  set(_libusb1_names libusb-1.0.a libusb-1.0 usb-1.0)
else()
  set(_libusb1_names usb-1.0 libusb-1.0 usb)
endif()

find_library(LibUSB1_LIBRARY
  NAMES ${_libusb1_names}
  HINTS ${PC_LIBUSB1_LIBDIR} ${PC_LIBUSB1_LIBRARY_DIRS}
  PATHS
    /opt/homebrew/lib
    /usr/local/lib
    /opt/local/lib
    /usr/lib
)

set(LibUSB1_VERSION ${PC_LIBUSB1_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibUSB1
  REQUIRED_VARS LibUSB1_LIBRARY LibUSB1_INCLUDE_DIR
  VERSION_VAR LibUSB1_VERSION
)

if(LibUSB1_FOUND)
  set(LibUSB1_INCLUDE_DIRS ${LibUSB1_INCLUDE_DIR})
  set(LibUSB1_LIBRARIES ${LibUSB1_LIBRARY})

  # A static libusb does not pull in its own dependencies, so name them here.
  set(_libusb1_extra_libs "")
  if(USBPROG_STATIC_LIBUSB)
    if(APPLE)
      list(APPEND _libusb1_extra_libs
        "-framework IOKit" "-framework CoreFoundation" "-framework Security" objc)
    elseif(UNIX)
      find_package(Threads REQUIRED)
      list(APPEND _libusb1_extra_libs Threads::Threads)
      find_library(LibUSB1_UDEV_LIBRARY udev)
      if(LibUSB1_UDEV_LIBRARY)
        list(APPEND _libusb1_extra_libs ${LibUSB1_UDEV_LIBRARY})
      endif()
    endif()
    list(APPEND LibUSB1_LIBRARIES ${_libusb1_extra_libs})
  endif()

  if(NOT TARGET LibUSB1::LibUSB1)
    add_library(LibUSB1::LibUSB1 UNKNOWN IMPORTED)
    set_target_properties(LibUSB1::LibUSB1 PROPERTIES
      IMPORTED_LOCATION "${LibUSB1_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LibUSB1_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES "${_libusb1_extra_libs}"
    )
  endif()
endif()

mark_as_advanced(LibUSB1_INCLUDE_DIR LibUSB1_LIBRARY LibUSB1_UDEV_LIBRARY)
