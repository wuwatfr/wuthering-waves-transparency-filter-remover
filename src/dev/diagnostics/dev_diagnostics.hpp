// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <reshade.hpp>

#include "fade_primitive_detector.hpp"

namespace wuwa_tfr::dev {

extern std::atomic<std::uint64_t> g_discard_shader_count;
extern std::atomic<std::uint64_t> g_strict_spatial_dither_count;
extern std::atomic<std::uint64_t> g_ambiguous_spatial_dither_count;
extern std::atomic<std::uint64_t> g_fade_primitive_shader_count;
extern std::atomic<std::uint64_t> g_fade_primitive_instance_count;

std::string FadePrimitiveConsumers(
    const std::vector<wuwa_tfr::FadePrimitiveInstance>& instances);

extern std::filesystem::path g_dump_path;
std::filesystem::path DumpDir();

std::string Hex64(std::uint64_t value);

}  // namespace wuwa_tfr::dev
