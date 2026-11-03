# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
# Builds the web version and uploads it to Cloudflare Pages (https://sculptureabc.pages.dev).
#   mobile\tools\deploy-web.ps1               build the multithreaded version (build\mobile-web-mt), then upload
#   mobile\tools\deploy-web.ps1 -SkipBuild    upload what build\mobile-web-mt already has
#   mobile\tools\deploy-web.ps1 -StageOnly    build build\site only (look before uploading)
#   mobile\tools\deploy-web.ps1 -Branch mt    upload as a preview instead (https://mt.sculptureabc.pages.dev)
#   mobile\tools\deploy-web.ps1 -SingleThread the single-threaded version (build\mobile-web, the LAN test build)
# The site is multithreaded (its headers make the page cross-origin isolated).
# First time on a PC: "npx wrangler@4 login" (opens the browser to sign in to Cloudflare).
# Only the page, its .js/.wasm and the header rules go up (build\site), not CMake's leftovers.
param([switch]$SkipBuild, [switch]$StageOnly, [switch]$SingleThread, [string]$Branch = 'main', [string]$Project = 'sculptureabc')
$ErrorActionPreference = 'Stop'
$Threads = -not $SingleThread
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$web = Join-Path $root $(if ($Threads) { 'build\mobile-web-mt' } else { 'build\mobile-web' })
$site = Join-Path $root 'build\site'

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-web.ps1') -Threads:$Threads
    if (-not $?) { throw 'build failed' }
}

if (Test-Path $site) { Remove-Item -Recurse -Force $site }
New-Item -ItemType Directory $site | Out-Null
foreach ($f in 'SculptureABC.html', 'SculptureABC.js', 'SculptureABC.wasm') {
    $src = Join-Path $web $f
    if (-not (Test-Path $src)) { throw "missing $src (build first)" }
    Copy-Item $src $site
}
# The site's front page is the app itself (SculptureABC.html still works too).
Copy-Item (Join-Path $web 'SculptureABC.html') (Join-Path $site 'index.html')
Copy-Item (Join-Path $root 'mobile\web\_headers') $site
Get-ChildItem $site | ForEach-Object { '{0,12:N0}  {1}' -f $_.Length, $_.Name }
if ($StageOnly) { return }

$commit = (& git -C $root rev-parse --short HEAD).Trim()
$dirty = if (& git -C $root status --porcelain) { ' (uncommitted changes)' } else { '' }
Push-Location (Join-Path $root 'build') # wrangler keeps a .wrangler cache folder where it runs
& npx --yes wrangler@4 pages deploy $site --project-name $Project --branch $Branch `
    --commit-hash (& git -C $root rev-parse HEAD).Trim() --commit-message "$commit$dirty" --commit-dirty=true
$ok = $LASTEXITCODE -eq 0
Pop-Location
if (-not $ok) { throw 'upload failed (signed in? "npx wrangler@4 login"; project made? first time: "npx wrangler@4 pages project create sculptureabc --production-branch main")' }
$url = if ($Branch -eq 'main') { "https://$Project.pages.dev/" } else { "https://$Branch.$Project.pages.dev/" }
Write-Host "Uploaded: $url"
