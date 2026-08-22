// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/diagnostics/dev_diagnostics.hpp"

#include "production/addon_shared.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

std::atomic<std::uint64_t> g_discard_shader_count{0};
std::atomic<std::uint64_t> g_strict_spatial_dither_count{0};
std::atomic<std::uint64_t> g_ambiguous_spatial_dither_count{0};
std::atomic<std::uint64_t> g_fade_primitive_shader_count{0};
std::atomic<std::uint64_t> g_fade_primitive_instance_count{0};

bool FindDxilPixelShader(
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects,
    const shader_desc*& descriptor,
    std::uint64_t& shader_hash) {
  descriptor = nullptr;
  shader_hash = 0;
  if (!subobjects) return false;

  for (std::uint32_t i = 0; i < subobject_count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::pixel_shader ||
        !subobjects[i].data)
      continue;
    const auto& candidate =
        *static_cast<const shader_desc*>(subobjects[i].data);
    if (!LooksLikeDxil(candidate)) continue;

    const std::uint64_t candidate_hash =
        Fnv1a64(candidate.code, candidate.code_size);
    if (descriptor && shader_hash != candidate_hash) return false;
    descriptor = &candidate;
    shader_hash = candidate_hash;
  }
  return descriptor != nullptr;
}

}  // namespace wuwa_tfr::dev
