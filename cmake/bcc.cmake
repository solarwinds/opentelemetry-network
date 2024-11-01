# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

include_guard()

# Find the system-installed BCC package
find_package(PkgConfig REQUIRED)
pkg_check_modules(BCC REQUIRED libbcc)

# Add BCC include directories and libraries
set(BCC_INCLUDE_DIRS ${BCC_INCLUDE_DIRS})
set(BCC_LIBS ${BCC_LIBRARIES})

# Output found paths for debugging
message(STATUS "Found BCC include dirs: ${BCC_INCLUDE_DIRS}")
message(STATUS "Found BCC libraries: ${BCC_LIBS}")
