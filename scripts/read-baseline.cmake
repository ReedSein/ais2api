# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

if(NOT DEFINED AIS2API_MANIFEST)
  message(FATAL_ERROR "Pass AIS2API_MANIFEST")
endif()
file(READ "${AIS2API_MANIFEST}" manifest)
string(JSON baseline GET "${manifest}" builtin-baseline)
string(LENGTH "${baseline}" baseline_length)
if(NOT baseline_length EQUAL 40 OR NOT baseline MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "vcpkg builtin-baseline must be a full commit SHA")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${baseline}" COMMAND_ERROR_IS_FATAL ANY)
