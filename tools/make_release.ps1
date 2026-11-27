param([string]$Tag = 'v1.0.0-rc1')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out'
$stage = Join-Path $out 'release-stage'
$zip = Join-Path $out ("mcla-launcher-$Tag.zip")

$exe = Join-Path $out 'mcla_launcher.exe'
if (-not (Test-Path $exe)) { throw "Missing release file: $exe" }

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item $exe $stage

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
Write-Output "RELEASE_ZIP=$zip"
Write-Output "SHA256=$hash"
Write-Output "BYTES=$((Get-Item $zip).Length)"
