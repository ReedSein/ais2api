# Copyright (c) 2026 ais2api contributors
# SPDX-License-Identifier: MPL-2.0
option(AIS2API_BUILD_WEBUI "Build and embed the Vue management console (requires Node.js/npm)" OFF)
if(AIS2API_BUILD_WEBUI)
  find_program(AIS2API_NODE NAMES node REQUIRED)
  find_program(AIS2API_NPM NAMES npm.cmd npm REQUIRED)
  execute_process(COMMAND "${AIS2API_NODE}" -e
    "const [a,b]=process.versions.node.split('.').map(Number); if(!((a===20&&b>=19)||(a===22&&b>=12)||a>22))process.exit(1)"
    RESULT_VARIABLE node_version_result)
  if(NOT node_version_result EQUAL 0)
    message(FATAL_ERROR "WebUI requires Node.js 20.19+ (20.x), or 22.12+; use AIS2API_BUILD_WEBUI=OFF for a native-only build")
  endif()
  set(webui_dir "${CMAKE_CURRENT_SOURCE_DIR}/webui")
  set(webui_deps "${webui_dir}/node_modules/.ais2api-deps")
  add_custom_command(OUTPUT "${webui_deps}"
    COMMAND "${AIS2API_NPM}" ci --no-audit --no-fund
    COMMAND "${CMAKE_COMMAND}" -E touch "${webui_deps}"
    DEPENDS "${webui_dir}/package.json" "${webui_dir}/package-lock.json"
    WORKING_DIRECTORY "${webui_dir}" VERBATIM)
  file(GLOB_RECURSE webui_sources CONFIGURE_DEPENDS "${webui_dir}/src/*" "${webui_dir}/public/*")
  set(webui_assets "${CMAKE_CURRENT_BINARY_DIR}/generated/webui_assets.inc")
  add_custom_command(OUTPUT "${webui_assets}"
    COMMAND "${AIS2API_NPM}" run build
    COMMAND "${AIS2API_NODE}" embed.mjs dist "${webui_assets}"
    DEPENDS "${webui_deps}" ${webui_sources} "${webui_dir}/index.html"
      "${webui_dir}/vite.config.ts" "${webui_dir}/tsconfig.json" "${webui_dir}/embed.mjs"
    WORKING_DIRECTORY "${webui_dir}" VERBATIM)
  add_custom_target(ais2api_webui DEPENDS "${webui_assets}")
  add_dependencies(ais2api_core ais2api_webui)
  target_compile_definitions(ais2api_core PRIVATE AIS2API_BUILD_WEBUI)
  target_include_directories(ais2api_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  install(FILES "${webui_dir}/public/THIRD_PARTY_LICENSES.txt" DESTINATION share/licenses/ais2api-webui)
endif()
