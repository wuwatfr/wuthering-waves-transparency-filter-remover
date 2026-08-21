# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 WuwaTFR contributors

cmake_minimum_required(VERSION 3.20)
project(WuwaTFR LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# Capture source revision at configure time so the add-on can report precisely
# which commit produced a binary, without requiring Git on an end-user system.
set(WUWA_TFR_BUILD_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE WUWA_TFR_BUILD_COMMIT_RESULT
    OUTPUT_VARIABLE WUWA_TFR_BUILD_COMMIT_CANDIDATE
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(WUWA_TFR_BUILD_COMMIT_RESULT EQUAL 0 AND
      WUWA_TFR_BUILD_COMMIT_CANDIDATE MATCHES "^[0-9A-Fa-f]+$")
    set(WUWA_TFR_BUILD_COMMIT "${WUWA_TFR_BUILD_COMMIT_CANDIDATE}")
  endif()
endif()

add_executable(pipeline_replacement_state_tests tests/test_pipeline_replacement_state.cpp)
target_include_directories(pipeline_replacement_state_tests PRIVATE src)
target_link_libraries(pipeline_replacement_state_tests PRIVATE Threads::Threads)

add_executable(dxc_validation_policy_tests tests/test_dxc_validation_policy.cpp)
target_include_directories(dxc_validation_policy_tests PRIVATE src)

add_executable(device_activity_state_tests tests/test_device_activity_state.cpp)
target_include_directories(device_activity_state_tests PRIVATE src)
target_link_libraries(device_activity_state_tests PRIVATE Threads::Threads)

add_executable(dxil_dither_diagnostic_tests
  tests/test_dxil_dither_diagnostic.cpp
  src/dxil_dither_diagnostic.cpp)
target_include_directories(dxil_dither_diagnostic_tests PRIVATE src)

add_executable(fade_primitive_detector_tests
  tests/test_fade_primitive_detector.cpp
  src/fade_primitive_detector.cpp)
target_include_directories(fade_primitive_detector_tests PRIVATE src)

add_executable(fade_primitive_audit EXCLUDE_FROM_ALL
  tools/fade_primitive_audit.cpp
  src/fade_primitive_detector.cpp)
target_include_directories(fade_primitive_audit PRIVATE src)

add_executable(target_dither_bypass_tests
  tests/test_target_dither_bypass.cpp
  src/target_dither_bypass.cpp
  src/fade_primitive_detector.cpp)
target_include_directories(target_dither_bypass_tests PRIVATE src)

add_executable(trace_submission_identity_tests
  tests/test_trace_submission_identity.cpp)
target_include_directories(trace_submission_identity_tests PRIVATE src)

enable_testing()
add_test(NAME pipeline_replacement_state_tests COMMAND pipeline_replacement_state_tests)
add_test(NAME dxc_validation_policy_tests COMMAND dxc_validation_policy_tests)
add_test(NAME device_activity_state_tests COMMAND device_activity_state_tests)
add_test(NAME dxil_dither_diagnostic_tests COMMAND dxil_dither_diagnostic_tests)
add_test(NAME fade_primitive_detector_tests COMMAND fade_primitive_detector_tests)
add_test(NAME target_dither_bypass_tests COMMAND target_dither_bypass_tests)
add_test(NAME trace_submission_identity_tests COMMAND trace_submission_identity_tests)

if(WIN32)
  set(RESHADE_INCLUDE_DIR "" CACHE PATH "Directory containing reshade.hpp")
  set(DXC_INCLUDE_DIR "" CACHE PATH "Directory containing dxc/dxcapi.h")
  set(IMGUI_INCLUDE_DIR "" CACHE PATH "Directory containing imgui.h")

  if(RESHADE_INCLUDE_DIR AND DXC_INCLUDE_DIR AND IMGUI_INCLUDE_DIR)
    function(add_wuwa_addon TARGET_NAME OUTPUT_NAME DEVTOOLS_VALUE)
      set(addon_sources
        src/addon.cpp
        src/dxc_bridge.cpp
        src/fade_primitive_detector.cpp
        src/target_dither_bypass.cpp)
      if(DEVTOOLS_VALUE)
        list(APPEND addon_sources
          src/dxil_dither_diagnostic.cpp
          )
      else()
        list(APPEND addon_sources
          src/fade_primitive_runtime.cpp)
      endif()
      add_library(${TARGET_NAME} MODULE ${addon_sources})
      target_include_directories(${TARGET_NAME} PRIVATE
        src
        "${RESHADE_INCLUDE_DIR}"
        "${DXC_INCLUDE_DIR}"
        "${IMGUI_INCLUDE_DIR}"
      )
      target_compile_definitions(${TARGET_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WUWA_TFR_DEVTOOLS=${DEVTOOLS_VALUE}
        WUWA_TFR_BUILD_COMMIT=\"${WUWA_TFR_BUILD_COMMIT}\"
      )
      if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /Gy /Gw)
        target_link_options(${TARGET_NAME} PRIVATE /OPT:REF /OPT:ICF)
      endif()
      set_target_properties(${TARGET_NAME} PROPERTIES
        PREFIX ""
        SUFFIX ".addon64"
        OUTPUT_NAME "${OUTPUT_NAME}"
      )
    endfunction()

    # User-facing automatic Transparency Filter Remover runtime.
    add_wuwa_addon(WuwaTFR WuwaTFR 0)

    # Developer target reserved for diagnostics.
    add_wuwa_addon(WuwaTFRDev WuwaTFRDev 1)
  else()
    message(STATUS "Windows add-on targets disabled: set RESHADE_INCLUDE_DIR, DXC_INCLUDE_DIR and IMGUI_INCLUDE_DIR")
  endif()
endif()
