# Builds REDGraph (cmake --build build) and deploys it into a game install or a mod-manager mod folder:
#   <Target>/red4ext/plugins/REDGraph/REDGraph.dll
#   <Target>/bin/x64/plugins/cyber_engine_tweaks/mods/REDGraph/   (the CET window)
# -SelfTest also deploys REDGraphSelfTest.dll (+ REDGraphSelfTest.ini from selftest/ when none is there yet).
# Target: -Target, else $env:RG_DEPLOY_DIR - the game's root (holding bin/ and red4ext/) or an MO2 mod's root.
# Configure build/ once first (README "Building"). The game must be closed: it holds the DLL open.

param(
    [string]$Target = $env:RG_DEPLOY_DIR,
    [switch]$SelfTest,
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'
if (-not $Target) { throw 'no target: pass -Target <game or mod root> or set RG_DEPLOY_DIR' }
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) { throw "build\ is not configured - see README 'Building'" }

$cmd = "cmake --build `"$build`" --config $Config"
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'no cmake: run from a Developer shell or install Visual Studio with the C++ tools' }
    $cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && $cmd"
}
# vcvars64.bat prints a harmless "'vswhere.exe' is not recognized" on stderr; the exit code decides.
$ErrorActionPreference = 'Continue'
cmd /c "$cmd 2>&1"
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0) { throw "build failed ($code)" }

# Paths into the target take -LiteralPath: an MO2 mod folder may be named '[NoDelete] ...' and -Path reads brackets as
# a wildcard. The copy is VERIFIED by hash: with the game running it fails, and PowerShell 5.1 may not stop on it.
function Deploy-Plugin([string]$name) {
    $built = @((Join-Path $build "$Config\$name.dll"), (Join-Path $build "$name.dll")) |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $built) { throw "no $name.dll under build\" }
    $dir = Join-Path $Target "red4ext\plugins\$name"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $dst = Join-Path $dir "$name.dll"
    try { Copy-Item -LiteralPath $built -Destination $dst -Force -ErrorAction Stop }
    catch { throw "DEPLOY FAILED: $($_.Exception.Message)`nClose the game - it holds $name.dll open." }
    if ((Get-FileHash -LiteralPath $built).Hash -ne (Get-FileHash -LiteralPath $dst).Hash) {
        throw "DEPLOY DID NOT TAKE: $dst differs from $built"
    }
    Write-Host "plugin  -> $dst"
    return $dir
}

[void](Deploy-Plugin 'REDGraph')
$cet = Join-Path $Target 'bin\x64\plugins\cyber_engine_tweaks\mods\REDGraph'
New-Item -ItemType Directory -Force -Path $cet | Out-Null
Copy-Item -Path (Join-Path $repo 'cet\REDGraph\*') -Destination $cet -Recurse -Force -ErrorAction Stop
Write-Host "CET mod -> $cet"

if ($SelfTest) {
    $dir = Deploy-Plugin 'REDGraphSelfTest'
    $ini = Join-Path $dir 'REDGraphSelfTest.ini'
    if (-not (Test-Path -LiteralPath $ini)) {
        Copy-Item -LiteralPath (Join-Path $repo 'selftest\REDGraphSelfTest.ini') -Destination $ini
        Write-Host "ini     -> $ini (every test off - edit it)"
    }
} elseif (Test-Path -LiteralPath (Join-Path $Target 'red4ext\plugins\REDGraphSelfTest')) {
    Write-Host 'NOTE: REDGraphSelfTest is still deployed - remove red4ext\plugins\REDGraphSelfTest when done testing'
}
