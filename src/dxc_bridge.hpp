// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#ifdef _WIN32
#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wuwa_tfr {

struct DxilInspectionOutput {
  bool success = false;
  std::string original_ir;
  std::string error;
};

struct DxilAssemblyValidationOutput {
  bool assembly_succeeded = false;
  bool validation_succeeded = false;
};

class DxcBridge {
 public:
  explicit DxcBridge(const std::filesystem::path& addon_directory);
  ~DxcBridge();
  DxcBridge(const DxcBridge&) = delete;
  DxcBridge& operator=(const DxcBridge&) = delete;

  bool available() const;
  bool assembly_available() const;
  const std::string& init_error() const;
  const std::string& assembly_error() const;

  DxilInspectionOutput InspectShader(const void* code, std::size_t size);

  bool AssembleAndValidate(
      const std::string& llvm_ir,
      std::vector<std::uint8_t>& bytecode,
      std::string& error,
      DxilAssemblyValidationOutput& output);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  std::string init_error_;
  std::string assembly_error_;
};

} // namespace wuwa_tfr
#endif
