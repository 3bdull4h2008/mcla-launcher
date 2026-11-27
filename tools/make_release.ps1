param([string]$Tag = 'v1.0.0-rc1')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out'
$stage = Join-Path $out 'release-stage'
$zip = Join-Path $out ("mcla-launcher-$Tag.zip")

$required = @(
  (Join-Path $out 'mcla_launcher.exe'),
  (Join-Path $out 'WebView2Loader.dll'),
  (Join-Path $out 'launcher.ico'),
  (Join-Path $out 'launcher\ui\index.html')
)
foreach ($f in $required) { if (-not (Test-Path $f)) { throw "Missing release file: $f" } }

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item (Join-Path $out 'mcla_launcher.exe') $stage
Copy-Item (Join-Path $out 'WebView2Loader.dll') $stage
Copy-Item (Join-Path $out 'launcher.ico') $stage
Copy-Item -Recurse (Join-Path $out 'launcher') (Join-Path $stage 'launcher')

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
Write-Output "RELEASE_ZIP=$zip"
Write-Output "SHA256=$hash"
Write-Output "BYTES=$((Get-Item $zip).Length)"
