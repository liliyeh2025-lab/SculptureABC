# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
# Builds upstream's unit tests (sculptforge_tests) with Emscripten and runs them under Node.js, the way the web
# build's core runs (single-threaded, WebAssembly exceptions). Output: build\mobile-node\test-results.txt
#   mobile\tools\test-node.ps1                   all tests
#   mobile\tools\test-node.ps1 brush dyntopo     only tests whose name contains one of the words
#   mobile\tools\test-node.ps1 -NoBuild          run the last build again
param([switch]$NoBuild, [Parameter(ValueFromRemainingArguments = $true)] [string[]]$Filters)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$emsdk = if ($env:EMSDK) { $env:EMSDK } else { 'E:\AI\tools\emsdk' }
$toolchain = Join-Path $emsdk 'upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake'
if (-not (Test-Path $toolchain)) { throw "Emscripten not found: $toolchain (install emsdk first)" }
. (Join-Path $emsdk 'emsdk_env.ps1') *> $null

$build = Join-Path $root 'build\mobile-node'
if (-not $NoBuild) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    $cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $cmake = Join-Path $cmakeDir 'CMake\bin\cmake.exe'
    $ninja = Join-Path $cmakeDir 'Ninja\ninja.exe'
    & $cmake -S (Join-Path $root 'mobile') -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" -DCMAKE_BUILD_TYPE=Release -DSCULPTURE_BUILD_TESTS=ON
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
    & $cmake --build $build --target sculptforge_tests --parallel
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

$results = Join-Path $build 'test-results.txt'
Push-Location $build
& node --stack-size=8000 sculptforge_tests.js @Filters *>&1 | Tee-Object -FilePath $results
$code = $LASTEXITCODE
Pop-Location
Write-Host "Exit code $code; output in $results"
exit $code
