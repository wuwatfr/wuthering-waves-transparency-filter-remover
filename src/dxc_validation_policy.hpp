// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

namespace wuwa_tfr {

enum class DxcValidationOutcome {
  Accepted,
  ValidateCallFailed,
  MissingOperationResult,
  StatusCallFailed,
  ValidationRejected,
  ResultCallFailed,
  MissingValidatedBlob,
};

constexpr DxcValidationOutcome ClassifyDxcValidation(
    bool validate_call_succeeded,
    bool operation_result_present,
    bool status_call_succeeded,
    bool validation_status_succeeded,
    bool result_call_succeeded,
    bool validated_blob_present) noexcept {
  if (!validate_call_succeeded)
    return DxcValidationOutcome::ValidateCallFailed;
  if (!operation_result_present)
    return DxcValidationOutcome::MissingOperationResult;
  if (!status_call_succeeded)
    return DxcValidationOutcome::StatusCallFailed;
  if (!validation_status_succeeded)
    return DxcValidationOutcome::ValidationRejected;
  if (!result_call_succeeded)
    return DxcValidationOutcome::ResultCallFailed;
  if (!validated_blob_present)
    return DxcValidationOutcome::MissingValidatedBlob;
  return DxcValidationOutcome::Accepted;
}

} // namespace wuwa_tfr
