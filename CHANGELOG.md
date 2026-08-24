# Changelog

## Unreleased

## v1.1.0 - 2026-08-25

### Changed

- Pre-Fade patch now removes only the camera-fade operand while preserving the
  other Fade input.
- Dev Manual Capture and Fade-control now use the same submission-time
  boundary for capture membership, while values remain sampled at Draw
  recording time.
- Trace exports use collision-safe, timestamped, correlated file groups
  instead of fixed names that overwrite on re-export.
- `fade_primitive_audit` accepts an explicit `--concrete-trace` input.

### Fixed

- Avoid duplicate full analysis of the original shader before pre-Fade
  patching.
- Enforce finite descriptor-table CBV bounds for scalar Fade-control reads.
- Device-scope Fade-control mapped-resource and descriptor-table state.
- Unify Trace/Fade resource incarnation tracking on one canonical lifecycle
  owner.
- Surface Fade-control tracker capacity loss instead of silently degrading
  evidence.

### Internal

- Consolidate shared DXIL lexical parsing.
- Unify execution pipeline identity used by Trace, Manual Capture, and
  Fade-control.
- Remove per-Draw deep copies of full Fade diagnostic evidence and reduce
  Fade-control locking.
- Remove dead Dev `Diagnostic` configuration.

## v1.0.0 - 2026-08-23

### Changed

- Make fade-primitive detection and patching function-scoped, with exact SSA
  matching and fail-closed handling for ambiguous shader shapes.
- Authorize only structurally verified visibility/output consumers, including
  validated `SV_Target` RGB triplets.
- Deduplicate concurrent preparation of the same shader through single-flight
  work coordination.
- Unify Production and Dev on the same `FadePrimitiveRuntime` replacement
  implementation and lifecycle; Dev diagnostics, capture, and trace remain
  separate observation tooling.
- Split generic add-on bootstrap from compile-time-selected Production and Dev
  variant modules, removing build-mode conditionals from common runtime code.

### Fixed

- Accept real DXC threshold loads with valid trailing metadata and verified
  FMin/Saturate propagation, restoring Production matches without admitting
  unknown calls or ambiguous SSA paths.
- Retain published replacement pipeline states for the required D3D12 lifetime.
- Isolate runtime-generated replacement pipeline create/bind/destroy events from
  Dev trace and diagnostics so synthetic events are not mistaken for game work.
- Fix Production callback compilation guards after the matcher/runtime changes.

### Added

- Show the short build commit hash in the overlay for exact binary/source
  identification.
- Add optional, session-only Production memory telemetry, disabled by default,
  with 10-second sampling and safe disable/re-enable scheduling.
- Add complete DXIL assemble/validate regression coverage using the pinned DXC
  runtime and run the Windows test suite in both Debug and Release.

### Internal

- Remove the obsolete duplicated Dev replacement runtime, legacy bypass/recipe
  experiments, dead replacement state, and other post-unification leftovers.
- Keep Production as the canonical runtime while building Dev as Production plus
  optional diagnostics and investigation tools.

### Documentation

- Update the funding link.

## v1.0.0-rc.2 - 2026-08-12

### Changed

- Match the target process by executable path for safer activation.

### Documentation

- Clarify the project title and ReShade API license information.
