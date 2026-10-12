param(
  [Parameter(Mandatory)]
  [ValidateSet('26.05', '26.08')]
  [string]$Version
)

$ErrorActionPreference = 'Stop'
# The official launcher is identical in the two validated OpenUSD tags.
$expectedSha256 = '41c7d691bd2e8c11a70559d27c0d287ddca44e9d7d5fc00a53c806ccfa4ec032'
$workspace = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$directory = Join-Path $workspace ".ci/openusd-testusdview/$Version"
$launcher = Join-Path $directory 'testusdview.py'
New-Item -ItemType Directory -Force $directory | Out-Null
Invoke-WebRequest "https://raw.githubusercontent.com/PixarAnimationStudios/OpenUSD/v$Version/pxr/usdImaging/bin/testusdview/testusdview.py" -OutFile $launcher
if ((Get-FileHash -Algorithm SHA256 $launcher).Hash.ToLowerInvariant() -ne $expectedSha256) {
  throw "OpenUSD $Version testusdview checksum mismatch"
}
$env:MERLIN_TESTUSDVIEW_SCRIPT = $launcher
if ($env:GITHUB_ENV) {
  "MERLIN_TESTUSDVIEW_SCRIPT=$launcher" | Out-File -Append -Encoding utf8 $env:GITHUB_ENV
}
Write-Host "OpenUSD ${Version} testusdview: $launcher"
