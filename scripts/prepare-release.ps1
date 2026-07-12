param(
  [Parameter(Mandatory = $true)]
  [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
  [string]$Version,

  [ValidatePattern('^[0-9]{4}-[0-9]{2}-[0-9]{2}$')]
  [string]$Date = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd'),

  [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$sourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$arguments = @(
  "-DMERLIN_RELEASE_VERSION:STRING=$Version"
  "-DMERLIN_RELEASE_DATE:STRING=$Date"
  "-DMERLIN_SOURCE_DIR:PATH=$sourceDir"
)
if ($DryRun) {
  $arguments += '-DMERLIN_DRY_RUN:BOOL=ON'
}
$arguments += @('-P', (Join-Path $sourceDir 'cmake/prepare-release.cmake'))

& cmake @arguments
if ($LASTEXITCODE -ne 0) {
  throw "release preparation failed with exit code $LASTEXITCODE"
}

if (-not $DryRun) {
  Write-Host "Review CHANGELOG.md, then commit and merge the release preparation."
  Write-Host "After main CI passes: git tag v$Version; git push origin v$Version"
}
