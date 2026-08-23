# WuwaTFR — Wuthering Waves Transparency Filter Remover

WuwaTFR is a ReShade add-on for Wuthering Waves (DirectX 12) that removes the
Transparency Filter applied to playable characters when the camera gets too
close.

## Important Notice

> [!WARNING]
> WuwaTFR is an unofficial third-party ReShade add-on that modifies the game's
> rendering pipeline at runtime.
>
> Use of ReShade, add-ons, or other third-party modifications may be incompatible
> with game updates, anti-cheat systems, or the game's rules, and may cause
> crashes, rendering issues, account restrictions, or other unintended results.
>
> **Use WuwaTFR entirely at your own risk.**
>
> This project is not affiliated with, endorsed by, or supported by Kuro Games
> or the ReShade project. No guarantee is made that use of WuwaTFR is permitted
> by the game operator or safe for any game account.
>
> You are responsible for deciding whether to use the software and for any
> consequences arising from its use.

The software is provided without warranty. See the [LICENSE](LICENSE) for the
full license terms.

## Features

- Removes the camera-proximity Transparency Filter.
- Uses automatic runtime structural detection for verified pixel shaders.
- Handles newly created character, form, skill, and animation pipeline states.
- Uses no character-specific or shader-hash allowlist.
- Provides a runtime **Remove Transparency Filter** toggle.
- Retains original game pipeline states for immediate fallback.
- Fails closed when verification, patching, assembly, validation, or
  replacement-pipeline creation fails.

## Requirements

- Wuthering Waves running in DX12 mode on 64-bit Windows.
- A working ReShade installation configured to load add-ons.

This project does not prescribe a ReShade version or a game installation path.

## Installation

The Production package contains these required runtime files:

- `WuwaTFR.addon64`
- `WuwaTFR.ini`
- `WuwaTFR.dxcompiler.dll`

Keep these runtime files together in the directory from which ReShade loads
`WuwaTFR.addon64`. ReShade's `AddonPath` may be customized; the add-on does
not need to be beside the Wuthering Waves executable. Copying only
`WuwaTFR.addon64` is insufficient.

The package also includes `LICENSE`, `NOTICE`, and the DXC runtime license
files described in [Third-Party Software](#third-party-software).

## Usage

Open the ReShade overlay and use **Remove Transparency Filter** in the
WuwaTFR section.

- **Enabled:** matching replacement pipeline states are used.
- **Disabled:** the game's original pipeline states are used immediately.

The setting can be changed at runtime without restarting the game. Its value
is stored in `WuwaTFR.ini` as `EnableTFR`.

## Optional Memory Telemetry

The Production overlay also has a **Log memory telemetry (10 s)** control for
long-session diagnostics. It is off by default on every process launch and is
not stored in `WuwaTFR.ini`. When enabled, it writes one schema/start line and
one sample every fixed 10 seconds to `ReShade.log`; a continuously enabled
five-hour session produces about 1,801 sample lines.

The process metrics (`working_set_bytes`, `private_commit_bytes`, and
`handle_count`) describe the entire game process, including the game, ReShade,
add-ons, and user-mode driver allocations. The WuwaTFR values are deliberately
narrower: they cover only the completed shader cache, its retained patched
bytecode payloads, in-flight preparations, tracked live replacement pipelines,
and active devices. `shader_cache_entries` includes cached fail-closed
outcomes, while `shader_cache_bytecode_bytes` includes only retained patched
bytecode vectors. The activity totals are cumulative counts since the
Production runtime started and are not changed by telemetry.

Therefore, process private commit growing while WuwaTFR's explicitly tracked
retention values remain flat is evidence against those caches and replacement
pipelines being responsible. It does not prove that the add-on contributes no
memory elsewhere.

## Reporting Visual Regressions

When reporting a visual regression, compare the same scene with **Remove
Transparency Filter** enabled and disabled.

## Building

On Windows, run:

```cmd
build_windows.cmd
```

The script downloads its declared build-time dependencies into an ignored
`_deps` directory and produces Production and Dev builds. The Production output
is the user-facing artifact; Dev is for local developer diagnostics.

## How It Works

WuwaTFR observes graphics pipeline creation through ReShade. Pixel shaders
that match the verified structural pattern are prepared with a narrowly scoped
patch, and replacement pipeline states are created while the original game
pipeline states are retained. The runtime toggle selects the original or
replacement state, and newly created matching pipeline states are handled
dynamically.

## License

WuwaTFR is licensed under **GNU General Public License version 3 only**
(`GPL-3.0-only`). You may use, study, modify, and redistribute the project
under GPLv3. Distributed modified or derivative versions must comply with
GPLv3, including corresponding-source obligations where applicable.

## Third-Party Software

- **ReShade API headers** — dual-licensed under BSD-3-Clause OR MIT; WuwaTFR
  uses them under the MIT license. The required notice is in [`NOTICE`](NOTICE).
- **Microsoft DirectX Shader Compiler runtime** — the build script packages
  the pinned official `dxcompiler.dll` as `WuwaTFR.dxcompiler.dll` from the
  official
  `v1.9.2602.24` redistributable. Its release notes identify
  `LICENSE-LLVM.txt` as applying to all archive files other than
  `d3d12shader.h`. To retain the complete accompanying upstream materials, the
  package includes the unmodified `LICENSE-LLVM.txt`, `LICENSE-MIT.txt`, and
  `LICENSE-MS.txt` files beside the runtime DLLs.
- **Dear ImGui** — MIT license. WuwaTFR uses its API headers for the overlay;
  it is not separately shipped in the Production package. Its complete notice
  is in [`NOTICE`](NOTICE).
