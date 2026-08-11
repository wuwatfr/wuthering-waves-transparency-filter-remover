// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dxc_validation_policy.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include "test_check.hpp"
#include <iostream>

using wuwa_tfr::ClassifyDxcValidation;
using wuwa_tfr::DxcValidationOutcome;

int main() {
  CHECK(ClassifyDxcValidation(false, false, false, false, false, false) ==
      DxcValidationOutcome::ValidateCallFailed);
  CHECK(ClassifyDxcValidation(true, false, false, false, false, false) ==
      DxcValidationOutcome::MissingOperationResult);
  CHECK(ClassifyDxcValidation(true, true, false, false, false, false) ==
      DxcValidationOutcome::StatusCallFailed);
  CHECK(ClassifyDxcValidation(true, true, true, false, false, false) ==
      DxcValidationOutcome::ValidationRejected);
  CHECK(ClassifyDxcValidation(true, true, true, true, false, false) ==
      DxcValidationOutcome::ResultCallFailed);
  CHECK(ClassifyDxcValidation(true, true, true, true, true, false) ==
      DxcValidationOutcome::MissingValidatedBlob);
  CHECK(ClassifyDxcValidation(true, true, true, true, true, true) ==
      DxcValidationOutcome::Accepted);
  std::cout << "DXC validation fail-closed policy tests passed\n";
  return 0;
}
