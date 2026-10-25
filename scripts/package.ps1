$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$binary = Join-Path $root 'build/release/Spotlight.exe'
if (!(Test-Path -LiteralPath $binary)) { throw 'Run scripts/build.ps1 first.' }
$destination = Join-Path $root 'dist'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath $binary -Destination (Join-Path $destination 'Spotlight.exe') -Force
Get-Item -LiteralPath (Join-Path $destination 'Spotlight.exe') | Select-Object FullName, Length
Get-FileHash -LiteralPath (Join-Path $destination 'Spotlight.exe') -Algorithm SHA256
