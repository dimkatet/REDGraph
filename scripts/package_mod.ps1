# Packages the mod for players: dist/REDGraph-<version>.zip in the game folder's layout (unzip into the game's root, or
# install with a mod manager):
#   red4ext/plugins/REDGraph/REDGraph.dll
#   bin/x64/plugins/cyber_engine_tweaks/mods/REDGraph/init.lua
# Build first (cmake --build build --config Release). The self-test and the example are not part of a release.

param([string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

$ver = (Select-String -Path (Join-Path $repo 'CMakeLists.txt') -Pattern 'project\(REDGraph VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$dll = @((Join-Path $repo "build\$Config\REDGraph.dll"), (Join-Path $repo 'build\REDGraph.dll')) |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $dll) { throw 'no build\...\REDGraph.dll - build first' }

$stage = Join-Path $repo "dist\REDGraph-$ver"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$plugin = Join-Path $stage 'red4ext\plugins\REDGraph'
$cet = Join-Path $stage 'bin\x64\plugins\cyber_engine_tweaks\mods\REDGraph'
New-Item -ItemType Directory -Force -Path $plugin, $cet | Out-Null
Copy-Item -LiteralPath $dll -Destination $plugin
Copy-Item -Path (Join-Path $repo 'cet\REDGraph\*') -Destination $cet -Recurse

$zip = Join-Path $repo "dist\REDGraph-$ver.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "mod -> $zip ($([math]::Round((Get-Item -LiteralPath $zip).Length / 1KB)) KB)"
