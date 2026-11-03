# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
# Builds the WebAssembly speed check into build\mobile-web\wasm-speed\ (open /wasm-speed/ on the test server).
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$emsdk = if ($env:EMSDK) { $env:EMSDK } else { 'E:\AI\tools\emsdk' }
. (Join-Path $emsdk 'emsdk_env.ps1') *> $null
$out = Join-Path $root 'build\mobile-web\wasm-speed'
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'index.html') $out -Force
$src = Join-Path $PSScriptRoot 'speed.cpp'
$variants = [ordered]@{
    base  = @()
    simd  = @('-msimd128')
    eh    = @('-fwasm-exceptions', '-DUSE_EH')
    mem2g = @('-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=2GB')
    mem4g = @('-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=4GB')
    app   = @('-msimd128', '-fwasm-exceptions', '-DUSE_EH', '-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=4GB')
}
foreach ($name in $variants.Keys) {
    $flags = @('-O3', $src, '-o', (Join-Path $out "$name.js"), '-sMODULARIZE=1', "-sEXPORT_NAME=make_$name",
        '-sEXPORTED_FUNCTIONS=_runFloat,_runInt', '-sENVIRONMENT=web', '-sINITIAL_MEMORY=64MB') + $variants[$name]
    & em++ @flags
    if ($LASTEXITCODE -ne 0) { throw "build failed: $name" }
    Write-Host "built $name"
}
