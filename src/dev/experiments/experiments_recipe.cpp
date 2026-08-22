// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/experiments/experiments_recipe.hpp"

#include <cstring>

using namespace reshade::api;

namespace wuwa_tfr::dev {

std::unordered_map<TracePipelineKey,
    std::shared_ptr<const TargetBypassPipelineRecipe>, TracePipelineKeyHash>
    g_target_bypass_recipes;

bool IsTargetBypassShaderSubobject(pipeline_subobject_type type) noexcept {
  switch (type) {
    case pipeline_subobject_type::vertex_shader:
    case pipeline_subobject_type::hull_shader:
    case pipeline_subobject_type::domain_shader:
    case pipeline_subobject_type::geometry_shader:
    case pipeline_subobject_type::pixel_shader:
      return true;
    default:
      return false;
  }
}

std::size_t TargetBypassRawElementSize(pipeline_subobject_type type) noexcept {
  switch (type) {
    case pipeline_subobject_type::stream_output_state:
      return sizeof(stream_output_desc);
    case pipeline_subobject_type::blend_state:
      return sizeof(blend_desc);
    case pipeline_subobject_type::rasterizer_state:
      return sizeof(rasterizer_desc);
    case pipeline_subobject_type::depth_stencil_state:
      return sizeof(depth_stencil_desc);
    case pipeline_subobject_type::primitive_topology:
      return sizeof(primitive_topology);
    case pipeline_subobject_type::depth_stencil_format:
    case pipeline_subobject_type::render_target_formats:
      return sizeof(format);
    case pipeline_subobject_type::sample_mask:
    case pipeline_subobject_type::sample_count:
    case pipeline_subobject_type::viewport_count:
    case pipeline_subobject_type::max_vertex_count:
      return sizeof(std::uint32_t);
    case pipeline_subobject_type::dynamic_pipeline_states:
      return sizeof(dynamic_state);
    case pipeline_subobject_type::flags:
      return sizeof(pipeline_flags);
    default:
      return 0;
  }
}

std::shared_ptr<const TargetBypassPipelineRecipe> CopyTargetBypassRecipe(
    DeviceIdentity device_key,
    pipeline_layout layout,
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects,
    pipeline application_pipeline,
    std::uint64_t shader_hash,
    std::string& error) {
  if (!subobjects || application_pipeline.handle == 0) {
    error = "missing pipeline creation description";
    return {};
  }

  auto recipe = std::make_shared<TargetBypassPipelineRecipe>();
  recipe->device = device_key;
  recipe->layout = layout;
  recipe->application_pipeline = application_pipeline;
  recipe->shader_hash = shader_hash;
  recipe->subobjects.reserve(subobject_count);

  for (std::uint32_t i = 0; i < subobject_count; ++i) {
    const pipeline_subobject& source = subobjects[i];
    TargetBypassRecipeSubobject destination;
    destination.type = source.type;
    destination.count = source.count;
    if (source.count == 0) {
      recipe->subobjects.push_back(std::move(destination));
      continue;
    }
    if (!source.data) {
      error = "pipeline subobject has null data";
      return {};
    }

    if (IsTargetBypassShaderSubobject(source.type)) {
      const auto* shaders = static_cast<const shader_desc*>(source.data);
      destination.shaders.reserve(source.count);
      for (std::uint32_t j = 0; j < source.count; ++j) {
        const shader_desc& shader = shaders[j];
        if ((shader.code_size != 0 && !shader.code) ||
            (shader.spec_constants != 0 &&
                (!shader.spec_constant_ids || !shader.spec_constant_values))) {
          error = "shader subobject has incomplete data";
          return {};
        }
        TargetBypassOwnedShader owned;
        const auto* code = static_cast<const std::uint8_t*>(shader.code);
        if (shader.code_size != 0)
          owned.code.assign(code, code + shader.code_size);
        if (shader.entry_point) owned.entry_point = shader.entry_point;
        if (shader.spec_constants != 0) {
          owned.spec_constant_ids.assign(shader.spec_constant_ids,
              shader.spec_constant_ids + shader.spec_constants);
          owned.spec_constant_values.assign(shader.spec_constant_values,
              shader.spec_constant_values + shader.spec_constants);
        }
        destination.shaders.push_back(std::move(owned));
      }
    } else if (source.type == pipeline_subobject_type::input_layout) {
      const auto* elements = static_cast<const input_element*>(source.data);
      destination.input_elements.reserve(source.count);
      for (std::uint32_t j = 0; j < source.count; ++j) {
        TargetBypassOwnedInputElement owned;
        owned.value = elements[j];
        if (elements[j].semantic) owned.semantic = elements[j].semantic;
        owned.value.semantic = nullptr;
        destination.input_elements.push_back(std::move(owned));
      }
    } else {
      const std::size_t element_size = TargetBypassRawElementSize(source.type);
      if (element_size == 0) {
        error = "unsupported pipeline subobject in cached recipe";
        return {};
      }
      destination.raw_data.resize(element_size * source.count);
      std::memcpy(destination.raw_data.data(), source.data,
          destination.raw_data.size());
    }
    recipe->subobjects.push_back(std::move(destination));
  }
  return recipe;
}

bool MaterializeTargetBypassRecipe(
    const TargetBypassPipelineRecipe& recipe,
    TargetBypassMaterializedRecipe& materialized,
    std::string& error) {
  materialized.subobjects.reserve(recipe.subobjects.size());
  materialized.shaders.reserve(recipe.subobjects.size());
  materialized.input_layouts.reserve(recipe.subobjects.size());
  materialized.raw_storage.reserve(recipe.subobjects.size());

  for (const auto& source : recipe.subobjects) {
    pipeline_subobject destination{source.type, source.count, nullptr};
    if (source.count == 0) {
      materialized.subobjects.push_back(destination);
      continue;
    }
    if (IsTargetBypassShaderSubobject(source.type)) {
      if (source.shaders.size() != source.count) {
        error = "cached shader subobject count mismatch";
        return false;
      }
      auto& shaders = materialized.shaders.emplace_back();
      shaders.resize(source.count);
      for (std::uint32_t i = 0; i < source.count; ++i) {
        const auto& owned = source.shaders[i];
        shader_desc& shader = shaders[i];
        shader.code = owned.code.empty() ? nullptr : owned.code.data();
        shader.code_size = owned.code.size();
        shader.entry_point = owned.entry_point.empty()
            ? nullptr : owned.entry_point.c_str();
        shader.spec_constants = static_cast<std::uint32_t>(
            owned.spec_constant_ids.size());
        shader.spec_constant_ids = owned.spec_constant_ids.empty()
            ? nullptr : owned.spec_constant_ids.data();
        shader.spec_constant_values = owned.spec_constant_values.empty()
            ? nullptr : owned.spec_constant_values.data();
      }
      destination.data = shaders.data();
      if (source.type == pipeline_subobject_type::pixel_shader) {
        if (source.count != 1 || materialized.pixel_shader != nullptr) {
          error = "cached recipe has an ambiguous pixel shader descriptor";
          return false;
        }
        materialized.pixel_shader = &shaders.front();
      }
    } else if (source.type == pipeline_subobject_type::input_layout) {
      if (source.input_elements.size() != source.count) {
        error = "cached input-layout count mismatch";
        return false;
      }
      auto& elements = materialized.input_layouts.emplace_back();
      elements.resize(source.count);
      for (std::uint32_t i = 0; i < source.count; ++i) {
        elements[i] = source.input_elements[i].value;
        elements[i].semantic = source.input_elements[i].semantic.empty()
            ? nullptr : source.input_elements[i].semantic.c_str();
      }
      destination.data = elements.data();
    } else {
      const std::size_t words =
          (source.raw_data.size() + sizeof(std::max_align_t) - 1) /
          sizeof(std::max_align_t);
      if (words == 0) {
        error = "cached raw subobject has no data";
        return false;
      }
      auto& storage = materialized.raw_storage.emplace_back(words);
      std::memcpy(storage.data(), source.raw_data.data(),
          source.raw_data.size());
      destination.data = storage.data();
    }
    materialized.subobjects.push_back(destination);
  }
  if (!materialized.pixel_shader) {
    error = "cached recipe has no pixel shader descriptor";
    return false;
  }
  return true;
}

}  // namespace wuwa_tfr::dev
