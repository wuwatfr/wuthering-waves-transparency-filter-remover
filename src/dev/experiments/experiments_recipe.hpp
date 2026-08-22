// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only deep-copy cache of a pipeline's creation recipe. ReShade only
// exposes a pipeline's creation description during init_pipeline; Dev keeps
// a process-local copy so both pipeline-replacement experiments
// (experiments_legacy_bypass.*, experiments_fade_primitive.*) can recreate an
// existing application PSO after the fact instead of waiting for the game to
// create it again.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <reshade.hpp>

#include "dev/trace/trace_state.hpp"
#include "production/addon_shared.hpp"

namespace wuwa_tfr::dev {

struct TargetBypassOwnedShader {
  std::vector<std::uint8_t> code;
  std::string entry_point;
  std::vector<std::uint32_t> spec_constant_ids;
  std::vector<std::uint32_t> spec_constant_values;
};

struct TargetBypassOwnedInputElement {
  reshade::api::input_element value;
  std::string semantic;
};

struct TargetBypassRecipeSubobject {
  reshade::api::pipeline_subobject_type type =
      reshade::api::pipeline_subobject_type::unknown;
  std::uint32_t count = 0;
  std::vector<std::byte> raw_data;
  std::vector<TargetBypassOwnedShader> shaders;
  std::vector<TargetBypassOwnedInputElement> input_elements;
};

struct TargetBypassPipelineRecipe {
  DeviceIdentity device = 0;
  reshade::api::pipeline_layout layout{};
  reshade::api::pipeline application_pipeline{};
  std::uint64_t shader_hash = 0;
  std::vector<TargetBypassRecipeSubobject> subobjects;
};

struct TargetBypassMaterializedRecipe {
  std::vector<reshade::api::pipeline_subobject> subobjects;
  std::vector<std::vector<reshade::api::shader_desc>> shaders;
  std::vector<std::vector<reshade::api::input_element>> input_layouts;
  std::vector<std::vector<std::max_align_t>> raw_storage;
  reshade::api::shader_desc* pixel_shader = nullptr;
};

// Keyed by the live application pipeline handle; entries are pruned whenever
// the owning trace PSO incarnation is destroyed or rotated out.
extern std::unordered_map<TracePipelineKey,
    std::shared_ptr<const TargetBypassPipelineRecipe>, TracePipelineKeyHash>
    g_target_bypass_recipes;

bool IsTargetBypassShaderSubobject(
    reshade::api::pipeline_subobject_type type) noexcept;
std::size_t TargetBypassRawElementSize(
    reshade::api::pipeline_subobject_type type) noexcept;

std::shared_ptr<const TargetBypassPipelineRecipe> CopyTargetBypassRecipe(
    DeviceIdentity device_key,
    reshade::api::pipeline_layout layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline application_pipeline,
    std::uint64_t shader_hash,
    std::string& error);

bool MaterializeTargetBypassRecipe(
    const TargetBypassPipelineRecipe& recipe,
    TargetBypassMaterializedRecipe& materialized,
    std::string& error);

}  // namespace wuwa_tfr::dev
