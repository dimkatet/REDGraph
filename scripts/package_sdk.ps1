# Packages the REDGraph SDK for modders: dist/REDGraph-SDK-<version>/ + .zip (version = project(REDGraph VERSION ...) in
# CMakeLists.txt). Sources: sdk/ (README, tools, reference, examples - regenerate the reference first with
# scripts/sdk_node_reference.py), include/rg_host.h, LICENSE. The package's layout is sdk/'s, plus include/.
# -BuildExample: configures and builds the PACKAGED example on its own - proof the package builds without this
# repository. Needs MSVC + CMake (a Developer shell, or Visual Studio found through vswhere); -Red4extSdk <checkout>
# skips the download of RED4ext.SDK.

param(
    [switch]$BuildExample,
    [string]$Red4extSdk = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

$ver = (Select-String -Path (Join-Path $repo 'CMakeLists.txt') -Pattern 'project\(REDGraph VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$name = "REDGraph-SDK-$ver"
$dist = Join-Path $repo 'dist'
$out = Join-Path $dist $name
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }

$files = [ordered]@{
    'README.md'                                 = 'sdk\README.md'
    'LICENSE'                                   = 'LICENSE'
    'include\rg_host.h'                         = 'include\rg_host.h'
    'examples\rg_client_example\client.cpp'     = 'sdk\examples\rg_client_example\client.cpp'
    'examples\rg_client_example\CMakeLists.txt' = 'sdk\examples\rg_client_example\CMakeLists.txt'
    'tools\rg_shader_cache.py'                  = 'sdk\tools\rg_shader_cache.py'
    'tools\rg_dxil.py'                          = 'sdk\tools\rg_dxil.py'
    'tools\space0_rebind.py'                    = 'sdk\tools\space0_rebind.py'
    'tools\rt_raygen_variant.py'                = 'sdk\tools\rt_raygen_variant.py'
    'reference\nodes.md'                        = 'sdk\reference\nodes.md'
    'reference\node_shaders.csv'                = 'sdk\reference\node_shaders.csv'
}
foreach ($k in $files.Keys) {
    $src = Join-Path $repo $files[$k]
    if (-not (Test-Path -LiteralPath $src)) { throw "missing: $($files[$k])" }
    $dst = Join-Path $out $k
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst
}

$zip = Join-Path $dist "$name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip
Write-Host "SDK -> $out"
Write-Host "zip -> $zip ($([math]::Round((Get-Item -LiteralPath $zip).Length / 1KB)) KB)"

if ($BuildExample) {
    $ex = Join-Path $out 'examples\rg_client_example'
    # Short on purpose: RED4ext.SDK's generated headers exceed MAX_PATH under a deep build folder.
    $bld = Join-Path $env:SystemDrive '\rg_sdk_example_build'
    if (Test-Path -LiteralPath $bld) { Remove-Item -LiteralPath $bld -Recurse -Force }
    $sdkArg = if ($Red4extSdk) { "-DRED4EXT_SDK_DIR=`"$((Resolve-Path $Red4extSdk).Path -replace '\\', '/')`"" } else { '' }
    $cmd = "cmake -S `"$ex`" -B `"$bld`" -A x64 $sdkArg && cmake --build `"$bld`" --config Release"
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $vs) { throw 'no MSVC: run from a Developer shell or install the VC tools' }
        $cmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && $cmd"
    }
    # vcvars64.bat prints a harmless "'vswhere.exe' is not recognized" on stderr; under Stop, PowerShell 5.1 would turn
    # it into a terminating NativeCommandError. The exit code decides.
    $ErrorActionPreference = 'Continue'
    cmd /c "$cmd 2>&1"
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "the packaged example did not build" }
    $dll = Join-Path $bld 'Release\REDGraphExampleClient.dll'
    if (-not (Test-Path -LiteralPath $dll)) { throw "no $dll" }
    Write-Host "packaged example builds: $dll"
    Remove-Item -LiteralPath $bld -Recurse -Force
}
