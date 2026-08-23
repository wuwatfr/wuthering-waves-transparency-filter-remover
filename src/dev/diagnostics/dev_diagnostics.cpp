// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/diagnostics/dev_diagnostics.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

#include "dev/dev_inspection.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

std::atomic<std::uint64_t> g_discard_shader_count{0};
std::atomic<std::uint64_t> g_strict_spatial_dither_count{0};
std::atomic<std::uint64_t> g_ambiguous_spatial_dither_count{0};
std::atomic<std::uint64_t> g_fade_primitive_shader_count{0};
std::atomic<std::uint64_t> g_fade_primitive_instance_count{0};
std::filesystem::path g_dump_path;

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

std::string FadePrimitiveConsumers(
    const wuwa_tfr::FadePrimitiveDiagnostic& diagnostic) {
  std::vector<const char*> names;
  for (const auto& instance : diagnostic.instances) {
    const char* name = wuwa_tfr::FadePrimitiveConsumerName(instance.consumer);
    if (std::find(names.begin(), names.end(), name) == names.end())
      names.push_back(name);
  }
  std::string result;
  for (const char* name : names) {
    if (!result.empty()) result += ", ";
    result += name;
  }
  return result.empty() ? "unknown" : result;
}

std::filesystem::path DumpDir() {
  if (g_dump_path.empty()) return {};
  std::error_code error;
  std::filesystem::create_directories(g_dump_path, error);
  if (error || !std::filesystem::is_directory(g_dump_path, error) || error)
    return {};
  return g_dump_path;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << std::uppercase << std::setw(16)
         << std::setfill('0') << value;
  return stream.str();
}

}  // namespace wuwa_tfr::dev
