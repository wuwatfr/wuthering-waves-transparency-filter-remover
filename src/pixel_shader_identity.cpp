// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "pixel_shader_identity.hpp"

#ifdef _WIN32
#include <cstring>

namespace wuwa_tfr {
namespace {

std::uint64_t Fnv1a64(const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

bool HasDxilChunk(const void* code, std::size_t size) {
  if (!code || size < 32) return false;
  const auto* bytes = static_cast<const std::uint8_t*>(code);
  if (std::memcmp(bytes, "DXBC", 4) != 0) return false;
  const auto read_u32 = [bytes](std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
  };
  const std::uint32_t total_size = read_u32(24);
  const std::uint32_t chunk_count = read_u32(28);
  if (total_size != size || total_size < 32 ||
      chunk_count > (total_size - 32) / 4)
    return false;
  bool has_dxil = false;
  for (std::uint32_t i = 0; i < chunk_count; ++i) {
    const std::uint32_t offset = read_u32(32 + 4 * i);
    if (offset > total_size || total_size - offset < 8) return false;
    const std::uint32_t chunk_size = read_u32(offset + 4);
    if (chunk_size > total_size - offset - 8) return false;
    has_dxil = has_dxil || std::memcmp(bytes + offset, "DXIL", 4) == 0;
  }
  return has_dxil;
}

}  // namespace

bool FindDxilPixelShader(std::uint32_t count,
    const reshade::api::pipeline_subobject* subobjects,
    const reshade::api::shader_desc*& shader, std::uint64_t& hash) {
  shader = nullptr;
  hash = 0;
  if (!subobjects) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type != reshade::api::pipeline_subobject_type::pixel_shader ||
        !subobjects[i].data)
      continue;
    const auto& candidate =
        *static_cast<const reshade::api::shader_desc*>(subobjects[i].data);
    if (!HasDxilChunk(candidate.code, candidate.code_size)) continue;
    const std::uint64_t candidate_hash =
        Fnv1a64(candidate.code, candidate.code_size);
    if (shader && hash != candidate_hash) return false;
    shader = &candidate;
    hash = candidate_hash;
  }
  return shader != nullptr;
}

}  // namespace wuwa_tfr
#endif
