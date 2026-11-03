# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
# Configures and builds the Windows development build with Visual Studio 2026's own CMake.
#   mobile\tools\build-win.ps1            Release
#   mobile\tools\build-win.ps1 -Debug     Debug
param([switch]$Debug)
# Not 'Stop': CMake writes warnings to stderr, which Windows PowerShell 5.1 would turn into errors.
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw "Visual Studio CMake not found under $vs" }
$build = Join-Path $root 'build\mobile-win'
# The Chinese font carries only the interface's characters: rebuild it when mobile/i18n changed (needs fontTools).
& python (Join-Path $root 'mobile\tools\make-font-subset.py') --if-needed
if ($LASTEXITCODE -ne 0) { Write-Warning 'font subset not rebuilt (python / fontTools missing?)' }

$config = if ($Debug) { 'Debug' } else { 'Release' }
& $cmake -S (Join-Path $root 'mobile') -B $build
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& $cmake --build $build --config $config --parallel
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host "Built: $build\$config\SculptureABC.exe"
