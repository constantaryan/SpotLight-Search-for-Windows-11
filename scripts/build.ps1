param([ValidateSet('release','debug')][string]$Configuration='release')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if(-not(Test-Path $vswhere)){throw 'Install Visual Studio Build Tools with C++ tools, CMake and Windows SDK.'}
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs){throw 'MSVC tools not found.'}
$dev=Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
Push-Location $root
try {
    $buildCommand = '"{0}" -arch=x64 -host_arch=x64 && cmake --preset {1} && cmake --build --preset {1} && ctest --preset {1}' -f $dev,$Configuration
    & cmd.exe /d /s /c $buildCommand
    if($LASTEXITCODE -ne 0){throw "Build/test failed: $LASTEXITCODE"}
} finally {Pop-Location}