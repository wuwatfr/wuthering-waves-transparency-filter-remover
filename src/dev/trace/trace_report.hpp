// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dev/trace/trace_state.hpp"
#include "trace_submission_identity.hpp"

namespace wuwa_tfr::dev {

bool WriteTraceReport();

std::string LocalExportTimestamp();

bool WriteCurrentInvestigationRange(
    const std::vector<ConcreteTraceRow>& frozen_rows,
    const std::vector<std::size_t>& displayed_indices,
    wuwa_tfr::TraceCandidateRange range,
    wuwa_tfr::TraceInvestigationView investigation_view);

}  // namespace wuwa_tfr::dev
