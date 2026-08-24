// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#ifdef _WIN32
#include <reshade.hpp>

#include <cstddef>
#include <cstdint>

namespace wuwa_tfr {

bool FindDxilPixelShader(std::uint32_t count,
    const reshade::api::pipeline_subobject* subobjects,
    const reshade::api::shader_desc*& shader, std::uint64_t& hash);

}
#endif
