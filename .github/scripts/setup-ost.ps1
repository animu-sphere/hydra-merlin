param(
  [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
  [string]$Version = '0.19.0'
)

$ErrorActionPreference = 'Stop'
$asset = 'ost-cli-x86_64-pc-windows-msvc.zip'
$base = "https://github.com/animu-sphere/open-strata/releases/download/v$Version"
$workspace = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$bootstrapRoot = Join-Path $workspace '.ost-ci/bootstrap-bin'

New-Item -ItemType Directory -Force $bootstrapRoot | Out-Null
Invoke-WebRequest "$base/$asset" -OutFile $asset
Invoke-WebRequest "$base/$asset.sha256" -OutFile "$asset.sha256"
$actual = (Get-FileHash -Algorithm SHA256 $asset).Hash.ToLowerInvariant()
$published = ((Get-Content "$asset.sha256" -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
if ($actual -ne $published) {
  throw "$asset hashes to $actual but the release publishes $published"
}

Expand-Archive -LiteralPath $asset -DestinationPath $bootstrapRoot -Force
$ost = Get-ChildItem $bootstrapRoot -Recurse -Filter ost.exe | Select-Object -First 1
if (-not $ost) {
  throw "no ost.exe inside $asset"
}

$env:PATH = "$($ost.Directory.FullName);$env:PATH"
if ($env:GITHUB_PATH) {
  $ost.Directory.FullName | Out-File -Append -Encoding utf8 $env:GITHUB_PATH
}
@{ schema = 1; version = $Version; asset = $asset; sha256 = $actual } |
  ConvertTo-Json -Compress |
  Out-File -Encoding utf8 (Join-Path $workspace '.ost-ci/bootstrap.json')

& $ost.FullName --version
