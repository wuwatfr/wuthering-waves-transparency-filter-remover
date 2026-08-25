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

# Derive the reported version from the nearest v* tag, so the tag that
# publishes a build stays its single source of truth. A build taken off a tag
# keeps the distance and commit git describe reports instead of claiming to be
# that release.
set(WUWA_TFR_VERSION "0.0.0-unknown")
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE WUWA_TFR_VERSION_RESULT
    OUTPUT_VARIABLE WUWA_TFR_VERSION_CANDIDATE
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(WUWA_TFR_VERSION_RESULT EQUAL 0 AND
      WUWA_TFR_VERSION_CANDIDATE MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+")
    string(REGEX REPLACE "^v" ""
      WUWA_TFR_VERSION "${WUWA_TFR_VERSION_CANDIDATE}")
  endif()
endif()

add_executable(pipeline_generation_state_tests tests/test_pipeline_generation_state.cpp)
target_include_directories(pipeline_generation_state_tests PRIVATE src)
target_link_libraries(pipeline_generation_state_tests PRIVATE Threads::Threads)

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
  src/fade_primitive_detector.cpp
  src/pre_fade_fmin_analysis.cpp
  src/target_dither_bypass.cpp
  src/dxil_ir_lexer.cpp)
target_include_directories(fade_primitive_detector_tests PRIVATE src)

add_executable(dxil_ir_lexer_tests
  tests/test_dxil_ir_lexer.cpp
  src/dxil_ir_lexer.cpp)
target_include_directories(dxil_ir_lexer_tests PRIVATE src)

add_executable(fade_primitive_audit EXCLUDE_FROM_ALL
  tools/fade_primitive_audit.cpp
  src/fade_primitive_detector.cpp
  src/dxil_ir_lexer.cpp
  src/trace_submission_presence.cpp)
target_include_directories(fade_primitive_audit PRIVATE src)

add_executable(trace_submission_presence_tests
  tests/test_trace_submission_presence.cpp
  src/trace_submission_presence.cpp)
target_include_directories(trace_submission_presence_tests PRIVATE src)

add_executable(pre_fade_fmin_audit EXCLUDE_FROM_ALL
  tools/pre_fade_fmin_audit.cpp
  src/fade_primitive_detector.cpp
  src/pre_fade_fmin_analysis.cpp
  src/target_dither_bypass.cpp
  src/dxil_ir_lexer.cpp)
target_include_directories(pre_fade_fmin_audit PRIVATE src)

add_executable(pre_fade_fmin_analysis_tests
  tests/test_pre_fade_fmin_analysis.cpp
  src/pre_fade_fmin_analysis.cpp
  src/target_dither_bypass.cpp
  src/fade_primitive_detector.cpp
  src/dxil_ir_lexer.cpp)
target_include_directories(pre_fade_fmin_analysis_tests PRIVATE src)

add_executable(target_dither_bypass_tests
  tests/test_target_dither_bypass.cpp
  src/target_dither_bypass.cpp
  src/pre_fade_fmin_analysis.cpp
  src/fade_primitive_detector.cpp
  src/dxil_ir_lexer.cpp)
target_include_directories(target_dither_bypass_tests PRIVATE src)

add_executable(fade_control_state_tests
  tests/test_fade_control_state.cpp
  src/dev/capture/fade_control_state.cpp)
target_include_directories(fade_control_state_tests PRIVATE src)

add_executable(descriptor_table_state_tests
  tests/test_descriptor_table_state.cpp
  src/dev/capture/descriptor_table_state.cpp
  # Device-teardown erasure must be provably store-only: the tests hold a
  # real FadeControlTrackerCapacityDiagnostics across it.
  src/dev/capture/fade_control_state.cpp)
target_include_directories(descriptor_table_state_tests PRIVATE src)

add_executable(fade_control_snapshot_tests
  tests/test_fade_control_snapshot.cpp
  src/dev/capture/fade_control_snapshot.cpp
  src/dev/capture/fade_control_state.cpp)
target_include_directories(fade_control_snapshot_tests PRIVATE src)

add_executable(trace_submission_identity_tests
  tests/test_trace_submission_identity.cpp)
target_include_directories(trace_submission_identity_tests PRIVATE src)

add_executable(manual_capture_state_tests
  tests/test_manual_capture_state.cpp
  src/dev/capture/manual_capture_state.cpp)
target_include_directories(manual_capture_state_tests PRIVATE src)

add_executable(fade_control_membership_tests
  tests/test_fade_control_membership.cpp
  src/dev/capture/fade_control_state.cpp
  src/dev/capture/fade_control_snapshot.cpp
  src/dev/capture/manual_capture_state.cpp)
target_include_directories(fade_control_membership_tests PRIVATE src)

add_executable(resource_lifecycle_state_tests
  tests/test_resource_lifecycle_state.cpp
  src/dev/resource_lifecycle_state.cpp)
target_include_directories(resource_lifecycle_state_tests PRIVATE src)

add_executable(single_flight_cache_tests tests/test_single_flight_cache.cpp)
target_include_directories(single_flight_cache_tests PRIVATE src)
target_link_libraries(single_flight_cache_tests PRIVATE Threads::Threads)

add_executable(memory_telemetry_tests tests/test_memory_telemetry.cpp)
target_include_directories(memory_telemetry_tests PRIVATE src)
target_link_libraries(memory_telemetry_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME pipeline_generation_state_tests COMMAND pipeline_generation_state_tests)
add_test(NAME dxc_validation_policy_tests COMMAND dxc_validation_policy_tests)
add_test(NAME device_activity_state_tests COMMAND device_activity_state_tests)
add_test(NAME dxil_dither_diagnostic_tests COMMAND dxil_dither_diagnostic_tests)
add_test(NAME fade_primitive_detector_tests COMMAND fade_primitive_detector_tests)
add_test(NAME dxil_ir_lexer_tests COMMAND dxil_ir_lexer_tests)
add_test(NAME pre_fade_fmin_analysis_tests COMMAND pre_fade_fmin_analysis_tests)
add_test(NAME target_dither_bypass_tests COMMAND target_dither_bypass_tests)
add_test(NAME fade_control_state_tests COMMAND fade_control_state_tests)
add_test(NAME descriptor_table_state_tests COMMAND descriptor_table_state_tests)
add_test(NAME fade_control_snapshot_tests COMMAND fade_control_snapshot_tests)
add_test(NAME trace_submission_identity_tests COMMAND trace_submission_identity_tests)
add_test(NAME manual_capture_state_tests COMMAND manual_capture_state_tests)
add_test(NAME fade_control_membership_tests COMMAND fade_control_membership_tests)
add_test(NAME resource_lifecycle_state_tests COMMAND resource_lifecycle_state_tests)
add_test(NAME single_flight_cache_tests COMMAND single_flight_cache_tests)
add_test(NAME trace_submission_presence_tests COMMAND trace_submission_presence_tests)
add_test(NAME memory_telemetry_tests COMMAND memory_telemetry_tests
  "${CMAKE_SOURCE_DIR}/src/production/production_module.cpp" "${CMAKE_SOURCE_DIR}/WuwaTFR.ini")

if(WIN32)
  set(RESHADE_INCLUDE_DIR "" CACHE PATH "Directory containing reshade.hpp")
  set(DXC_INCLUDE_DIR "" CACHE PATH "Directory containing dxc/dxcapi.h")
  set(IMGUI_INCLUDE_DIR "" CACHE PATH "Directory containing imgui.h")

  if(RESHADE_INCLUDE_DIR AND DXC_INCLUDE_DIR AND IMGUI_INCLUDE_DIR)
    set(DXC_RUNTIME_DIR "" CACHE PATH
      "Directory containing the pinned x64 dxcompiler.dll and dxil.dll")
    function(add_wuwa_addon TARGET_NAME OUTPUT_NAME DEVTOOLS_VALUE)
      set(addon_sources
        src/addon.cpp
        src/dxc_bridge.cpp
        src/dxil_ir_lexer.cpp
        src/fade_primitive_detector.cpp
        src/fade_primitive_runtime.cpp
        src/pixel_shader_identity.cpp
        src/pre_fade_fmin_analysis.cpp
        src/target_dither_bypass.cpp)
      if(DEVTOOLS_VALUE)
        list(APPEND addon_sources
          src/dxil_dither_diagnostic.cpp
          src/dev/capture/descriptor_table_state.cpp
          src/dev/capture/fade_control_runtime.cpp
          src/dev/capture/fade_control_snapshot.cpp
          src/dev/capture/fade_control_state.cpp
          src/dev/capture/manual_capture.cpp
          src/dev/capture/manual_capture_state.cpp
          src/dev/diagnostics/dev_diagnostics.cpp
          src/dev/resource_lifecycle_state.cpp
          src/dev/trace/trace_events.cpp
          src/dev/trace/trace_report.cpp
          src/dev/trace/trace_state.cpp
          src/dev/dev_events.cpp
          src/dev/dev_inspection.cpp
          src/dev/dev_module.cpp
          src/dev/dev_overlay.cpp
          src/dev/dev_runtime.cpp
          )
      else()
        list(APPEND addon_sources
          src/production/production_module.cpp)
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
        WUWA_TFR_VERSION=\"${WUWA_TFR_VERSION}\"
        WUWA_TFR_BUILD_COMMIT=\"${WUWA_TFR_BUILD_COMMIT}\"
      )
      if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /Gy /Gw)
        target_link_options(${TARGET_NAME} PRIVATE /OPT:REF /OPT:ICF)
      endif()
      target_link_libraries(${TARGET_NAME} PRIVATE Psapi)
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

    if(DXC_RUNTIME_DIR)
      add_executable(complete_dxil_validation_tests
        tests/test_complete_dxil_validation.cpp
        src/dxc_bridge.cpp
        src/fade_primitive_detector.cpp
        src/pre_fade_fmin_analysis.cpp
        src/target_dither_bypass.cpp
        src/dxil_ir_lexer.cpp)
      target_include_directories(complete_dxil_validation_tests PRIVATE
        src "${DXC_INCLUDE_DIR}")
      add_custom_command(TARGET complete_dxil_validation_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${DXC_RUNTIME_DIR}/dxcompiler.dll"
          "$<TARGET_FILE_DIR:complete_dxil_validation_tests>/WuwaTFR.dxcompiler.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${DXC_RUNTIME_DIR}/dxil.dll"
          "$<TARGET_FILE_DIR:complete_dxil_validation_tests>/dxil.dll")
      add_test(NAME complete_dxil_validation_tests
        COMMAND complete_dxil_validation_tests
          "${CMAKE_SOURCE_DIR}/tests/fixtures/fade_primitive_validation.ll")
    endif()
  else()
    message(STATUS "Windows add-on targets disabled: set RESHADE_INCLUDE_DIR, DXC_INCLUDE_DIR and IMGUI_INCLUDE_DIR")
  endif()
endif()
