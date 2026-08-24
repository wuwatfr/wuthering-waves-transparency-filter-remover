// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#ifdef _WIN32
#include <reshade.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fade_primitive_detector.hpp"
#include "target_dither_bypass.hpp"

namespace wuwa_tfr {

class FadePrimitiveRuntimeObserver {
 public:
  virtual ~FadePrimitiveRuntimeObserver() = default;

  struct ShaderPreparationObservation {
    std::uint64_t original_shader_hash = 0;
    std::size_t original_bytecode_size = 0;

    bool inspection_succeeded = false;
    std::string inspection_error;

    const std::string* original_ir = nullptr;

    FadePrimitiveDiagnostic fade_primitive;

    std::vector<PreFadeFMinEvidence> pre_fade_evidence;

    bool patch_succeeded = false;
    std::string patch_failure;

    bool prepared_succeeded = false;
    std::string prepared_failure;
  };

  virtual void OnShaderPrepared(const ShaderPreparationObservation&) {}

  struct PipelineInitObservation {
    reshade::api::device* device = nullptr;
    std::uint64_t application_pipeline = 0;

    bool pixel_shader_identified = false;
    std::uint64_t pixel_shader_hash = 0;
  };

  virtual void OnPipelineInit(const PipelineInitObservation&) {}
};

}
#endif
