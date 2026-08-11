// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#ifdef _WIN32
#include "dxc_bridge.hpp"
#include "dxc_validation_policy.hpp"

#include <Windows.h>
#include <Unknwn.h>
#include <ObjIdl.h>
#include <OAIdl.h>
#include <wrl/client.h>
#include <dxc/dxcapi.h>

#include <filesystem>
#include <sstream>

namespace wuwa_tfr {
using Microsoft::WRL::ComPtr;

struct DxcBridge::Impl {
  HMODULE module = nullptr;
  DxcCreateInstanceProc create_instance = nullptr;
  ComPtr<IDxcUtils> utils;
  ComPtr<IDxcCompiler3> compiler;
  ComPtr<IDxcAssembler> assembler;
  ComPtr<IDxcValidator> validator;
};

static std::string OperationErrors(IDxcOperationResult* result) {
  if (!result) return {};
  ComPtr<IDxcBlobEncoding> errors;
  if (FAILED(result->GetErrorBuffer(&errors)) || !errors ||
      errors->GetBufferSize() == 0)
    return {};
  return std::string(
      static_cast<const char*>(errors->GetBufferPointer()),
      errors->GetBufferSize());
}

static std::string Win32LoadError(
    const std::filesystem::path& path,
    DWORD error) {
  std::ostringstream oss;
  oss << "failed to load " << path.string() << " (Win32 error " << error << ")";
  if (error == ERROR_BAD_EXE_FORMAT) {
    oss << ": wrong DLL architecture (expected x64)";
  } else if (error == ERROR_MOD_NOT_FOUND) {
    oss << ": file or one of its dependencies was not found";
  }
  return oss.str();
}

static bool DisassembleDxil(
    IDxcCompiler3* compiler,
    const void* code,
    std::size_t size,
    std::string& ir,
    std::string& error) {
  if (!compiler || !code || size == 0) {
    error = "empty shader or unavailable DXC compiler";
    return false;
  }

  DxcBuffer object{};
  object.Ptr = code;
  object.Size = size;
  object.Encoding = DXC_CP_ACP;

  ComPtr<IDxcResult> disasm_result;
  const HRESULT call = compiler->Disassemble(
      &object, IID_PPV_ARGS(&disasm_result));
  if (FAILED(call) || !disasm_result) {
    error = "DXC disassembly call failed";
    return false;
  }

  HRESULT status = E_FAIL;
  if (FAILED(disasm_result->GetStatus(&status))) {
    error = "failed to query DXC disassembly status";
    return false;
  }
  if (FAILED(status)) {
    ComPtr<IDxcBlobUtf8> errors;
    disasm_result->GetOutput(
        DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    error = errors
        ? std::string(errors->GetStringPointer(), errors->GetStringLength())
        : "DXC disassembly failed";
    return false;
  }

  ComPtr<IDxcBlobUtf8> text_blob;
  if (FAILED(disasm_result->GetOutput(
          DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&text_blob), nullptr)) ||
      !text_blob) {
    error = "DXC did not return disassembly text";
    return false;
  }

  ir.assign(text_blob->GetStringPointer(), text_blob->GetStringLength());
  return true;
}

static bool AssembleDxil(
    IDxcUtils* utils,
    IDxcAssembler* assembler,
    IDxcValidator* validator,
    const std::string& text,
    std::vector<std::uint8_t>& bytecode,
    std::string& error,
    DxilAssemblyValidationOutput& output) {
  bytecode.clear();
  error.clear();
  output = {};
  if (!utils || !assembler || !validator) {
    error = "DXIL assembly requires utils, assembler, and validator";
    return false;
  }
  if (text.size() > UINT32_MAX) {
    error = "DXIL text is too large to assemble";
    return false;
  }

  ComPtr<IDxcBlobEncoding> source_blob;
  if (FAILED(utils->CreateBlob(
          text.data(), static_cast<UINT32>(text.size()), DXC_CP_UTF8,
          &source_blob)) ||
      !source_blob) {
    error = "failed to create UTF-8 DXIL text blob";
    return false;
  }

  ComPtr<IDxcOperationResult> assembly;
  const HRESULT assembly_call =
      assembler->AssembleToContainer(source_blob.Get(), &assembly);
  if (FAILED(assembly_call) || !assembly) {
    error = "DXIL assembler call failed";
    return false;
  }

  HRESULT assembly_status = E_FAIL;
  if (FAILED(assembly->GetStatus(&assembly_status))) {
    error = "failed to query DXIL assembly status";
    return false;
  }
  if (FAILED(assembly_status)) {
    error = "DXIL assembly failed: " + OperationErrors(assembly.Get());
    return false;
  }

  ComPtr<IDxcBlob> assembled;
  if (FAILED(assembly->GetResult(&assembled)) || !assembled) {
    error = "DXIL assembler returned no container";
    return false;
  }
  output.assembly_succeeded = true;

  ComPtr<IDxcOperationResult> validation;
  const HRESULT validate_call = validator->Validate(
      assembled.Get(), DxcValidatorFlags_Default, &validation);
  HRESULT validation_status = E_FAIL;
  HRESULT status_call = E_FAIL;
  HRESULT result_call = E_FAIL;
  ComPtr<IDxcBlob> validated;
  if (SUCCEEDED(validate_call) && validation) {
    status_call = validation->GetStatus(&validation_status);
    if (SUCCEEDED(status_call) && SUCCEEDED(validation_status))
      result_call = validation->GetResult(&validated);
  }

  switch (ClassifyDxcValidation(
      SUCCEEDED(validate_call), validation.Get() != nullptr,
      SUCCEEDED(status_call), SUCCEEDED(validation_status),
      SUCCEEDED(result_call), validated.Get() != nullptr)) {
    case DxcValidationOutcome::Accepted:
      break;
    case DxcValidationOutcome::ValidateCallFailed:
      error = "DXIL validator call failed";
      return false;
    case DxcValidationOutcome::MissingOperationResult:
      error = "DXIL validator returned no operation result";
      return false;
    case DxcValidationOutcome::StatusCallFailed:
      error = "failed to query DXIL validation status";
      return false;
    case DxcValidationOutcome::ValidationRejected:
      error = "DXIL validation failed: " + OperationErrors(validation.Get());
      return false;
    case DxcValidationOutcome::ResultCallFailed:
      error = "failed to obtain validated DXIL";
      return false;
    case DxcValidationOutcome::MissingValidatedBlob:
      error = "DXIL validator returned no validated container";
      return false;
  }

  output.validation_succeeded = true;

  const auto* data =
      static_cast<const std::uint8_t*>(validated->GetBufferPointer());
  bytecode.assign(data, data + validated->GetBufferSize());
  return true;
}

DxcBridge::DxcBridge(const std::filesystem::path& addon_directory)
    : impl_(new Impl) {
  if (addon_directory.empty()) {
    init_error_ = "WuwaTFR add-on directory is unavailable";
    return;
  }

  const auto compiler_path = addon_directory / L"WuwaTFR.dxcompiler.dll";
  if (GetFileAttributesW(compiler_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    init_error_ = "WuwaTFR private DXC runtime is missing: " +
        compiler_path.string();
    return;
  }

  impl_->module = LoadLibraryExW(
      compiler_path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!impl_->module) {
    init_error_ = Win32LoadError(compiler_path, GetLastError());
    return;
  }

  impl_->create_instance = reinterpret_cast<DxcCreateInstanceProc>(
      GetProcAddress(impl_->module, "DxcCreateInstance"));
  if (!impl_->create_instance) {
    init_error_ = "DxcCreateInstance export not found";
    return;
  }

  if (FAILED(impl_->create_instance(
          CLSID_DxcUtils, IID_PPV_ARGS(&impl_->utils))) ||
      FAILED(impl_->create_instance(
          CLSID_DxcCompiler, IID_PPV_ARGS(&impl_->compiler)))) {
    init_error_ = "failed to create DXC disassembly interfaces";
    return;
  }

  const bool assembler_ok = SUCCEEDED(impl_->create_instance(
      CLSID_DxcAssembler, IID_PPV_ARGS(&impl_->assembler)));
  const bool validator_ok = SUCCEEDED(impl_->create_instance(
      CLSID_DxcValidator, IID_PPV_ARGS(&impl_->validator)));
  if (!assembler_ok || !validator_ok) {
    assembly_error_ =
        "DXIL assembly disabled: assembler and validator are both required";
  }
}

DxcBridge::~DxcBridge() {
  if (!impl_) return;
  impl_->validator.Reset();
  impl_->assembler.Reset();
  impl_->compiler.Reset();
  impl_->utils.Reset();
  if (impl_->module) FreeLibrary(impl_->module);
  delete impl_;
}

bool DxcBridge::available() const {
  return impl_ && impl_->module && impl_->utils && impl_->compiler;
}

bool DxcBridge::assembly_available() const {
  return available() && impl_->assembler && impl_->validator;
}

const std::string& DxcBridge::init_error() const { return init_error_; }

const std::string& DxcBridge::assembly_error() const {
  return assembly_error_;
}

DxilInspectionOutput DxcBridge::InspectShader(
    const void* code,
    std::size_t size) {
  DxilInspectionOutput output;
  if (!available()) {
    output.error = init_error_.empty()
        ? "DXC disassembly unavailable"
        : init_error_;
    return output;
  }
  output.success = DisassembleDxil(
      impl_->compiler.Get(), code, size, output.original_ir, output.error);
  return output;
}

bool DxcBridge::AssembleAndValidate(
    const std::string& llvm_ir,
    std::vector<std::uint8_t>& bytecode,
    std::string& error,
    DxilAssemblyValidationOutput& output) {
  output = {};
  if (!assembly_available()) {
    error = assembly_error_.empty()
        ? "DXIL assembly unavailable"
        : assembly_error_;
    return false;
  }
  return AssembleDxil(
      impl_->utils.Get(), impl_->assembler.Get(), impl_->validator.Get(),
      llvm_ir, bytecode, error, output);
}

} // namespace wuwa_tfr
#endif
