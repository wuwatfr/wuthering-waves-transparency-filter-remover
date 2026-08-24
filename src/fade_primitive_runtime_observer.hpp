// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// A narrow, optional, read-only seam onto facts FadePrimitiveRuntime already
// computes on its existing canonical execution path -- introduced so a
// future diagnostic consumer (Dev) can eventually observe those facts
// instead of independently recomputing them with a second DXC pool and a
// second matcher. This header only defines the observer contract; nothing
// in this commit migrates any existing consumer onto it.

#pragma once

#ifdef _WIN32
#include <reshade.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fade_primitive_detector.hpp"
#include "target_dither_bypass.hpp"

namespace wuwa_tfr {

// Implemented by a diagnostic consumer, never by Production. An
// implementation must not mutate FadePrimitiveRuntime's own decisions --
// there is no return value or out-parameter through which it could, and it
// is never given a non-const handle to any runtime state -- and must not
// take ownership of, retain, or destroy any Production replacement pipeline.
// It also must not block for long: both callbacks are invoked synchronously
// on FadePrimitiveRuntime's existing shader-preparation and application-
// pipeline-initialization paths.
class FadePrimitiveRuntimeObserver {
 public:
  virtual ~FadePrimitiveRuntimeObserver() = default;

  // Everything FadePrimitiveRuntime's shader-preparation path already knows
  // about one shader by the time it finishes preparing it. Every field is
  // either copied from a value that path already produced, or a pointer
  // into one of its own locals valid only for the duration of this call --
  // nothing here is computed specially for this observation, and nothing is
  // retained by the runtime afterward purely to support it.
  struct ShaderPreparationObservation {
    std::uint64_t original_shader_hash = 0;
    std::size_t original_bytecode_size = 0;

    // Whether DxcBridge::InspectShader() -- or an earlier DXC pool/context
    // failure that meant InspectShader() was never even called -- succeeded,
    // and the failure text when it did not. `original_ir` and
    // `fade_primitive` are only meaningful when this is true.
    bool inspection_succeeded = false;
    std::string inspection_error;

    // Non-null, and pointing at the runtime's own already-disassembled IR
    // text, iff inspection_succeeded; null otherwise. Valid only for the
    // duration of this call -- the observer must not retain this pointer.
    const std::string* original_ir = nullptr;

    // The canonical Fade Primitive diagnostic the runtime itself matched
    // against. Default (empty) when inspection did not succeed.
    FadePrimitiveDiagnostic fade_primitive;

    // The canonical pre-Fade patch evidence (see TargetDitherBypassResult::
    // instance_evidence) from PatchAllVerifiedFadePrimitiveInstancesPreFade
    // Operand(), when the runtime called it -- i.e. when fade_primitive had
    // at least one instance. Empty when the patch was never invoked, and
    // (per instance_evidence's own documented semantics) possibly a strict
    // prefix of fade_primitive.instances when the patch itself failed
    // partway through.
    std::vector<PreFadeFMinEvidence> pre_fade_evidence;

    // Whether PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand() itself
    // succeeded, and its failure text when it did not. Both are their
    // defaults (false, empty) when the patch was never invoked.
    bool patch_succeeded = false;
    std::string patch_failure;

    // The runtime's own final outcome for this shader: whether a
    // replacement bytecode payload was produced at all (i.e. inspection,
    // matching, patching, and DXC assemble+validate every succeeded), and
    // the single failure text the runtime itself surfaces -- covering every
    // failure mode above plus the two this struct does not otherwise carry
    // a dedicated field for: "no fully verified transparency-filter
    // primitive" and an assemble/validate failure after a successful patch.
    bool prepared_succeeded = false;
    std::string prepared_failure;
  };

  // Called synchronously, exactly once per unique original shader hash --
  // the same granularity at which the runtime itself prepares a shader --
  // immediately before the call that computed it returns. Never called
  // again for a hash whose preparation is already cached.
  virtual void OnShaderPrepared(const ShaderPreparationObservation&) {}

  // Everything FadePrimitiveRuntime's application-pipeline-initialization
  // path already knows about one application pipeline by the point it has
  // identified (or failed to identify) that pipeline's pixel shader.
  struct PipelineInitObservation {
    // The reshade::api::device this application pipeline belongs to. An
    // opaque identity for the observer's own bookkeeping; never dereferenced
    // by the runtime on the observer's behalf, and the observer must not use
    // it to call back into device/pipeline-mutating ReShade API surface from
    // within this callback.
    reshade::api::device* device = nullptr;
    std::uint64_t application_pipeline = 0;

    // Exactly the runtime's own FindPixelShader() result: whether a single
    // DXIL pixel shader was identified among this pipeline's subobjects.
    // False for both "no DXIL pixel shader subobject present" and
    // "multiple pixel-shader subobjects with differing content hashes" --
    // the runtime's own matcher does not currently distinguish those two
    // cases from each other, so this observation cannot manufacture that
    // distinction either.
    bool pixel_shader_identified = false;
    // Meaningful only when pixel_shader_identified is true.
    std::uint64_t pixel_shader_hash = 0;
  };

  // Called synchronously from OnInitPipeline(), once per call that reaches
  // pixel-shader identification (i.e. every call for an active, still-
  // tracked device with a non-zero application pipeline handle), regardless
  // of what the runtime's own replacement-selection logic subsequently does
  // with the result.
  virtual void OnPipelineInit(const PipelineInitObservation&) {}
};

}  // namespace wuwa_tfr
#endif
