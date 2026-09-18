# WuwaTFR — Wuthering Waves Transparency Filter Remover

WuwaTFR is a ReShade add-on for Wuthering Waves (DirectX 12) that removes the
Transparency Filter applied to playable characters when the camera gets too
close. It needs no XXMI/WWMI, no 3DMigoto, and no per-character mod files.

## Video Demo

[![WuwaTFR: removing the Transparency Filter in Wuthering Waves (v1.1 demo)](https://img.youtube.com/vi/S5iVoR3A9pI/maxresdefault.jpg)](https://www.youtube.com/watch?v=S5iVoR3A9pI)

Watch the demo on YouTube:
<https://www.youtube.com/watch?v=S5iVoR3A9pI>

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

## FAQ

### Does it work in the DirectX 12 version of the game?

Yes. WuwaTFR only works in DX12 mode, and DLSS and ray tracing are supported
under DX12. It ignores devices that are not DirectX 12 and does nothing there.

### Do I need XXMI, WWMI, or 3DMigoto?

No. WuwaTFR is a ReShade add-on. It does not use 3DMigoto/WWMI, does not
need mod `.ini` files, and does not require a model-importer launcher.
Running it together with XXMI has not been tested.

### Do I need a separate file for each character?

No. Detection is structural: the add-on inspects each pixel shader's compiled
code for the Transparency Filter pattern instead of matching a list of shader
hashes or characters. New characters, forms, skills, and animation states whose
shaders carry the same pattern are handled automatically, including pipeline
states created after the game has started.

### Does it break on every game update?

Not by design. Because there is no hash or character allowlist, an update that
leaves the shader structure unchanged needs no new release. If an update
changes the structure so that a shader no longer matches, WuwaTFR fails
closed: that shader keeps the game's original rendering, and nothing is
patched speculatively.

### Does it change anything besides the Transparency Filter?

The patch is narrowly scoped to the validated camera-proximity fade
component; unrelated fade inputs in the same shader are preserved.

### Can I turn it off without restarting the game?

Yes. The **Remove Transparency Filter** toggle in the ReShade overlay switches
between the game's original pipeline states and the replacement states
immediately. See [Usage](#usage).

### Is it safe to use with the game's anti-cheat?

Unknown. WuwaTFR modifies rendering at runtime through ReShade. Read the
[Important Notice](#important-notice) and use it at your own risk.

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

`replacements_failed_total` counts only genuine failures: a matched shader
that could not be patched, assembled or validated; a preparation that could
not reach a verdict at all; a pixel-shader substitution that did not apply;
and a replacement pipeline state that could not be created. A pixel shader
that simply carries no transparency filter is the expected outcome for nearly
every shader in the game and is not counted.

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
