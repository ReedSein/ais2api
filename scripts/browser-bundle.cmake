# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Browser bundling is opt-in; only a clean, hash-inventoried runtime is accepted.
set(AIS2API_BROWSER_BUNDLE_DIR "" CACHE PATH "Clean browser runtime created by scripts/browser-bundle.py stage")
if(AIS2API_BROWSER_BUNDLE_DIR)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(AIS2API_BROWSER_BUNDLE_PLATFORM windows-x64)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(AIS2API_BROWSER_BUNDLE_PLATFORM linux-x64)
  else()
    message(FATAL_ERROR "Browser packages currently support Windows x64 and Linux x64 only")
  endif()
  get_filename_component(AIS2API_BROWSER_BUNDLE_DIR "${AIS2API_BROWSER_BUNDLE_DIR}" ABSOLUTE)
  set(AIS2API_BROWSER_VALIDATE_COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser-bundle.py" validate
    --manifest "${CMAKE_CURRENT_SOURCE_DIR}/runtime-manifest.json"
    --platform "${AIS2API_BROWSER_BUNDLE_PLATFORM}" --output "${AIS2API_BROWSER_BUNDLE_DIR}")
  execute_process(COMMAND ${AIS2API_BROWSER_VALIDATE_COMMAND} RESULT_VARIABLE bundle_result)
  if(NOT bundle_result EQUAL 0)
    message(FATAL_ERROR "Browser bundle validation failed; create it with scripts/browser-bundle.py stage")
  endif()
  # Recheck at install/CPack time so changes after configuration cannot enter a package.
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate-browser-install.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/validate-browser-install.cmake" @ONLY)
  install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/validate-browser-install.cmake")
  install(DIRECTORY "${AIS2API_BROWSER_BUNDLE_DIR}/"
    DESTINATION "bin/browser-runtime/${AIS2API_BROWSER_BUNDLE_PLATFORM}" USE_SOURCE_PERMISSIONS)
endif()
