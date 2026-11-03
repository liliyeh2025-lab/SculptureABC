# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
# Publishes the source on GitHub (origin) as a snapshot: the tree of `main` without the project's own notes, as one
# commit on the local branch `public`, pushed to origin's main. The site's code is public (GPL) while the
# development history and notes stay on this PC and the NAS.
#   mobile\tools\publish-source.ps1           make the snapshot and push it
#   mobile\tools\publish-source.ps1 -DryRun   make it, don't push (look with: git show --stat public)
param([switch]$DryRun)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
Push-Location $root
try {
    # Not published: notes and plans, and upstream's GitHub automation (it would start running on our repo).
    $private = @('PLAN.md', 'CHECKLIST.md', 'CLAUDE.md', 'mobile/UPSTREAM.md', '.github')

    if (git status --porcelain --untracked-files=no) {
        Write-Warning 'There are uncommitted changes: the snapshot is what main has committed, not the working tree.'
    }
    $index = Join-Path $env:TEMP 'sculptureabc-public.index'
    if (Test-Path $index) { Remove-Item $index }
    $env:GIT_INDEX_FILE = $index
    try {
        git read-tree main
        if ($LASTEXITCODE -ne 0) { throw 'read-tree failed' }
        git rm --cached -r -q --ignore-unmatch -- $private
        if ($LASTEXITCODE -ne 0) { throw 'rm failed' }
        # The front page: our read-me (mobile/README.public.md) as README.md, upstream's kept as README.SculptForge.md.
        $ours = git rev-parse main:mobile/README.public.md
        $upstream = git rev-parse main:README.md
        if ($LASTEXITCODE -ne 0) { throw 'README not found' }
        git rm --cached -q -- mobile/README.public.md README.md
        git update-index --add --cacheinfo "100644,$ours,README.md"
        git update-index --add --cacheinfo "100644,$upstream,README.SculptForge.md"
        if ($LASTEXITCODE -ne 0) { throw 'README swap failed' }
        $tree = git write-tree
        if ($LASTEXITCODE -ne 0) { throw 'write-tree failed' }
    } finally {
        Remove-Item Env:GIT_INDEX_FILE
        if (Test-Path $index) { Remove-Item $index }
    }

    $parent = git rev-parse -q --verify refs/heads/public 2>$null
    if ($parent -and (git rev-parse "$parent^{tree}") -eq $tree) {
        Write-Host 'The published source is already up to date.'
    } else {
        $message = 'Update ' + (Get-Date -Format 'yyyy-MM-dd')
        $commit = if ($parent) { git commit-tree $tree -p $parent -m $message } else { git commit-tree $tree -m $message }
        if ($LASTEXITCODE -ne 0) { throw 'commit-tree failed' }
        git update-ref refs/heads/public $commit
        Write-Host "Snapshot $commit ($message)"
    }
    if (-not $DryRun) {
        git push origin refs/heads/public:refs/heads/main
        if ($LASTEXITCODE -ne 0) { throw 'push failed' }
        Write-Host 'Published: https://github.com/liliyeh2025-lab/SculptureABC'
    }
} finally {
    Pop-Location
}
