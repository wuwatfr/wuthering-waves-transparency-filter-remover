// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dxc_bridge.hpp"
#include "fade_primitive_detector.hpp"
#include "target_dither_bypass.hpp"
#include "test_check.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  CHECK(argc == 2);
  std::ifstream input(argv[1], std::ios::binary);
  const std::string fixture(std::istreambuf_iterator<char>(input), {});
  CHECK(fixture.find("dxcoob 1.9.2602.24") != std::string::npos);
  CHECK(fixture.find(", !dx.controlflow.hints !") != std::string::npos);

  const auto analysis = wuwa_tfr::AnalyzeFadePrimitiveV1(fixture);
  CHECK(analysis.instances.size() == 1);
  const auto patch =
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(fixture);
  CHECK(patch.success);
  CHECK(patch.patched_instance_count == 1);
  // Unlike the retired identity-phi patch, the verified primitive itself is
  // untouched: only the pre-Fade FMin's operand 1 changes, so the same
  // instance must still be independently verifiable post-patch.
  CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(patch.llvm_ir).instances.size() == 1);

  wuwa_tfr::DxcBridge dxc(std::filesystem::path(argv[0]).parent_path());
  CHECK(dxc.available());
  CHECK(dxc.assembly_available());
  std::vector<std::uint8_t> bytecode;
  std::string error;
  wuwa_tfr::DxilAssemblyValidationOutput output;
  CHECK(dxc.AssembleAndValidate(patch.llvm_ir, bytecode, error, output));
  CHECK(output.assembly_succeeded);
  CHECK(output.validation_succeeded);
  CHECK(!bytecode.empty());
  return 0;
}
