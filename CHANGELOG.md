# Changelog

## Unreleased

### Changed

- Make fade-primitive detection and patching function-scoped, with exact SSA
  matching and fail-closed handling for ambiguous shader shapes.
- Recognize a verified `SV_Target` RGB triplet as an explicit safe fade
  consumer; other outputs remain rejected.
- Deduplicate concurrent preparation of the same shader through single-flight
  work coordination.

### Added

- Show the short build commit hash in the overlay.
- Add optional, session-only Production memory telemetry, disabled by default,
  with 10-second sampling and safe disable/re-enable scheduling.

### Documentation

- Update the funding link.

## v1.0.0-rc.2 - 2026-08-12

### Changed

- Match the target process by executable path for safer activation.

### Documentation

- Clarify the project title and ReShade API license information.
