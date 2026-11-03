# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
# Builds the web version (Emscripten -> build\mobile-web\SculptureABC.html/.js/.wasm).
# Needs emsdk (default E:\AI\tools\emsdk, or set $env:EMSDK) with "emsdk install latest" + "emsdk activate latest".
#   mobile\tools\build-web.ps1            Release
#   mobile\tools\build-web.ps1 -Debug     Debug (assertions, readable stack traces)
#   mobile\tools\build-web.ps1 -Threads   multithreaded (-pthread) -> build\mobile-web-mt; needs a cross-origin
#                                        isolated page (https + COOP/COEP: the Cloudflare site, or serve.py on localhost)
param([switch]$Debug, [switch]$Threads)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$emsdk = if ($env:EMSDK) { $env:EMSDK } else { 'E:\AI\tools\emsdk' }
$toolchain = Join-Path $emsdk 'upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake'
if (-not (Test-Path $toolchain)) { throw "Emscripten not found: $toolchain (install emsdk first)" }
. (Join-Path $emsdk 'emsdk_env.ps1') *> $null

# Visual Studio's CMake 4.3 and Ninja (the cmake on PATH is too old).
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $cmakeDir 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeDir 'Ninja\ninja.exe'

# The Chinese font carries only the interface's characters: rebuild it when mobile/i18n changed (needs fontTools).
& python (Join-Path $root 'mobile\tools\make-font-subset.py') --if-needed
if ($LASTEXITCODE -ne 0) { Write-Warning 'font subset not rebuilt (python / fontTools missing?)' }

$config = if ($Debug) { 'Debug' } else { 'Release' }
$build = Join-Path $root $(if ($Threads) { 'build\mobile-web-mt' } else { 'build\mobile-web' })
$threadsFlag = if ($Threads) { 'ON' } else { 'OFF' }
& $cmake -S (Join-Path $root 'mobile') -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DCMAKE_BUILD_TYPE=$config" "-DSCULPTURE_WEB_THREADS=$threadsFlag"
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& $cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host "Built: $build\SculptureABC.html"
