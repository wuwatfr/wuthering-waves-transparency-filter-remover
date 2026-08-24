# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 WuwaTFR contributors

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Keep compiler and CMake diagnostics readable in a Windows console.
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
$env:VSLANG = '1033'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$deps = Join-Path $root '_deps'
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
$distRelease = Join-Path $dist 'release'
$distDev = Join-Path $dist 'dev'

# Optional build-download proxy. Set WUWA_BUILD_PROXY to a proxy URI before
# running build_windows.cmd when one is required by the network.
$buildProxy = $env:WUWA_BUILD_PROXY

function Fail([string]$Message) {
    Write-Host ''
    Write-Host 'BUILD FAILED' -ForegroundColor Red
    Write-Host $Message -ForegroundColor Red
    Write-Host ''
    Write-Host 'Required once on this PC:'
    Write-Host '  Visual Studio 2026/2022 Build Tools'
    Write-Host '  Workload: Desktop development with C++'
    Write-Host '  Component: C++ CMake tools for Windows'
    exit 1
}

function Download-And-Expand([string]$Url, [string]$ZipPath, [string]$Destination) {
    if (Test-Path $Destination) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ZipPath) | Out-Null
    Write-Host "Downloading $Url"
    $requestArgs = @{ Uri = $Url; OutFile = $ZipPath; UseBasicParsing = $true }
    if ($buildProxy) {
        Write-Host "  via proxy $buildProxy"
        $requestArgs.Proxy = $buildProxy
    }
    Invoke-WebRequest @requestArgs
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Expand-Archive -Path $ZipPath -DestinationPath $Destination -Force
}

Write-Host '=== WuwaTFR Windows build ===' -ForegroundColor Cyan

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsInstall = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
        if ($vsInstall) {
            $bundledCmake = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $bundledCmake) {
                $cmake = Get-Command $bundledCmake
            }
        }
    }
}
if (-not $cmake) {
    Fail 'cmake.exe was not found in PATH or the installed Visual Studio Build Tools.'
}

New-Item -ItemType Directory -Force -Path $deps | Out-Null
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist, $distRelease, $distDev | Out-Null

# Pin all build-time dependencies, including the ReShade add-on API headers.
$reshadeRevision = 'aae2b7ecc18096ccddca2c073b50727541220292'
$reshadeUrl = "https://github.com/crosire/reshade/archive/$reshadeRevision.zip"
$dxcSourceUrl = 'https://github.com/microsoft/DirectXShaderCompiler/archive/refs/tags/v1.9.2602.24.zip'
$dxcBinaryUrl = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/dxc_2026_05_27.zip'
$imguiUrl = 'https://github.com/ocornut/imgui/archive/refs/tags/v1.92.5-docking.zip'

$reshadeRoot = Join-Path $deps 'reshade-src'
$dxcSourceRoot = Join-Path $deps 'dxc-src'
$dxcBinaryRoot = Join-Path $deps 'dxc-bin'
$imguiRoot = Join-Path $deps 'imgui-src'

Download-And-Expand $reshadeUrl (Join-Path $deps 'reshade.zip') $reshadeRoot
Download-And-Expand $dxcSourceUrl (Join-Path $deps 'dxc-source.zip') $dxcSourceRoot
Download-And-Expand $dxcBinaryUrl (Join-Path $deps 'dxc-binary.zip') $dxcBinaryRoot
Download-And-Expand $imguiUrl (Join-Path $deps 'imgui.zip') $imguiRoot

$reshadeHeader = Get-ChildItem -Path $reshadeRoot -Recurse -Filter 'reshade.hpp' -File | Select-Object -First 1
if (-not $reshadeHeader) { Fail 'Could not find reshade.hpp after downloading ReShade.' }
$reshadeInclude = $reshadeHeader.Directory.FullName

$dxcHeader = Get-ChildItem -Path $dxcSourceRoot -Recurse -Filter 'dxcapi.h' -File | Where-Object { $_.Directory.Name -eq 'dxc' } | Select-Object -First 1
if (-not $dxcHeader) { Fail 'Could not find dxc/dxcapi.h after downloading DXC.' }
$dxcInclude = $dxcHeader.Directory.Parent.FullName

$imguiHeader = Get-ChildItem -Path $imguiRoot -Recurse -Filter 'imgui.h' -File | Select-Object -First 1
if (-not $imguiHeader) { Fail 'Could not find imgui.h after downloading Dear ImGui v1.92.5-docking.' }
$imguiInclude = $imguiHeader.Directory.FullName

Write-Host "ReShade include: $reshadeInclude"
Write-Host "DXC include:     $dxcInclude"
Write-Host "ImGui include:   $imguiInclude"

if (Test-Path $build) { Remove-Item -Recurse -Force $build }

# Match the CMake Visual Studio generator to the installed Build Tools.
$generator = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $installVersion = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion | Select-Object -First 1)
    if ($installVersion) {
        $major = [int]($installVersion.Split('.')[0])
        if ($major -ge 18) { $generator = 'Visual Studio 18 2026' }
        elseif ($major -eq 17) { $generator = 'Visual Studio 17 2022' }
    }
}
if (-not $generator) {
    # Fallback for systems where vswhere is unavailable.
    $cmakeHelp = (& $cmake.Source --help | Out-String)
    if ($cmakeHelp -match 'Visual Studio 18 2026') { $generator = 'Visual Studio 18 2026' }
    elseif ($cmakeHelp -match 'Visual Studio 17 2022') { $generator = 'Visual Studio 17 2022' }
}
if (-not $generator) { Fail 'Could not determine a compatible Visual Studio CMake generator.' }

Write-Host "Configuring x64 Release build with: $generator"
& $cmake.Source -S $root -B $build -G $generator -A x64 `
    "-DRESHADE_INCLUDE_DIR=$reshadeInclude" `
    "-DDXC_INCLUDE_DIR=$dxcInclude" `
    "-DDXC_RUNTIME_DIR=$(Join-Path $dxcBinaryRoot 'bin/x64')" `
    "-DIMGUI_INCLUDE_DIR=$imguiInclude"
if ($LASTEXITCODE -ne 0) {
    Fail 'CMake configuration failed. Make sure the installed Visual Studio Build Tools include Desktop development with C++.'
}

$testTargets = @(
    'pipeline_generation_state_tests',
    'dxc_validation_policy_tests',
    'device_activity_state_tests',
    'dxil_dither_diagnostic_tests',
    'fade_primitive_detector_tests',
    'pre_fade_fmin_analysis_tests',
    'target_dither_bypass_tests',
    'fade_control_state_tests',
    'descriptor_table_state_tests',
    'fade_control_snapshot_tests',
    'trace_submission_identity_tests',
    'manual_capture_state_tests',
    'single_flight_cache_tests',
    'memory_telemetry_tests',
    'complete_dxil_validation_tests'
)

Write-Host 'Compiling Release test targets...'
& $cmake.Source --build $build --config Release --target $testTargets
if ($LASTEXITCODE -ne 0) { Fail 'Test compilation failed. See the compiler errors above.' }

$ctest = Join-Path (Split-Path -Parent $cmake.Source) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
    Fail 'ctest.exe was not found alongside cmake.exe.'
}

Write-Host 'Running all Release CTest tests...'
& $ctest --test-dir $build --build-config Release --output-on-failure
if ($LASTEXITCODE -ne 0) { Fail 'Tests failed; build verification did not succeed.' }

Write-Host 'Compiling Debug test targets...'
& $cmake.Source --build $build --config Debug --target $testTargets
if ($LASTEXITCODE -ne 0) { Fail 'Debug test compilation failed. See the compiler errors above.' }

Write-Host 'Running all Debug CTest tests...'
& $ctest --test-dir $build --build-config Debug --output-on-failure
if ($LASTEXITCODE -ne 0) { Fail 'Debug tests failed; build verification did not succeed.' }

Write-Host 'Compiling production add-on...'
& $cmake.Source --build $build --config Release --target WuwaTFR
if ($LASTEXITCODE -ne 0) { Fail 'Production add-on compilation failed. See the compiler errors above.' }

Write-Host 'Compiling developer add-on...'
& $cmake.Source --build $build --config Release --target WuwaTFRDev
if ($LASTEXITCODE -ne 0) { Fail 'Developer add-on compilation failed. See the compiler errors above.' }

$addon = Get-ChildItem -Path $build -Recurse -Filter 'WuwaTFR.addon64' -File | Select-Object -First 1
$devAddon = Get-ChildItem -Path $build -Recurse -Filter 'WuwaTFRDev.addon64' -File | Select-Object -First 1
if (-not $addon) { Fail 'Build succeeded but WuwaTFR.addon64 was not found.' }
if (-not $devAddon) { Fail 'Build succeeded but WuwaTFRDev.addon64 was not found.' }

Copy-Item $addon.FullName (Join-Path $distRelease 'WuwaTFR.addon64') -Force
# Keep the install filename identical inside the dev folder so users copy exactly one folder.
Copy-Item $devAddon.FullName (Join-Path $distDev 'WuwaTFR.addon64') -Force
Copy-Item (Join-Path $root 'WuwaTFR.ini') (Join-Path $distRelease 'WuwaTFR.ini') -Force
Copy-Item (Join-Path $root 'WuwaTFR.ini') (Join-Path $distDev 'WuwaTFR.ini') -Force
# Ship the project license and required third-party notices with both packages.
foreach ($documentName in @('LICENSE', 'NOTICE')) {
    $documentPath = Join-Path $root $documentName
    if (-not (Test-Path $documentPath)) { Fail "Required release document is missing: $documentName" }
    Copy-Item $documentPath (Join-Path $distRelease $documentName) -Force
    Copy-Item $documentPath (Join-Path $distDev $documentName) -Force
}

# The official DXC archive contains multiple architectures. Select x64 strictly.
$dxcompiler = Get-ChildItem -Path $dxcBinaryRoot -Recurse -Filter 'dxcompiler.dll' -File |
    Where-Object { $_.FullName -match '[\\/]x64[\\/]' } | Select-Object -First 1
if (-not $dxcompiler) { Fail 'Could not find the x64 dxcompiler.dll in the official DXC package.' }
Write-Host "DXC runtime x64: $($dxcompiler.FullName)"
Copy-Item $dxcompiler.FullName (Join-Path $distRelease 'WuwaTFR.dxcompiler.dll') -Force
Copy-Item $dxcompiler.FullName (Join-Path $distDev 'WuwaTFR.dxcompiler.dll') -Force
# Preserve the exact upstream license materials that accompany the DXC runtime.
foreach ($licenseName in @('LICENSE-LLVM.txt', 'LICENSE-MIT.txt', 'LICENSE-MS.txt')) {
    $licensePath = Join-Path $dxcBinaryRoot $licenseName
    if (-not (Test-Path $licensePath)) { Fail "Could not find $licenseName in the official DXC package." }
    Copy-Item $licensePath (Join-Path $distRelease $licenseName) -Force
    Copy-Item $licensePath (Join-Path $distDev $licenseName) -Force
}

@'
WuwaTFR Developer build
========================

Install only one folder at a time:

  dist\release  = normal user-facing automatic transparency-removal runtime
  dist\dev      = developer diagnostics build

The public release asset is the production add-on in dist\release together
with WuwaTFR.ini and WuwaTFR.dxcompiler.dll. Developer diagnostics are not
normal user features and are not part of the release package.
'@ | Set-Content -Encoding UTF8 (Join-Path $distDev 'FIRST_TEST.txt')

Write-Host ''
Write-Host 'BUILD SUCCEEDED' -ForegroundColor Green
Write-Host "Production: $distRelease" -ForegroundColor Green
Write-Host "Developer:  $distDev" -ForegroundColor Green
Write-Host ''
Write-Host 'Install only one add-on variant at a time.' -ForegroundColor Yellow
