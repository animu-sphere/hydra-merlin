param(
  [Parameter(Mandatory)]
  [ValidateSet('26.05', '26.08')]
  [string]$Version,
  [switch]$ValidateRuntime
)

$ErrorActionPreference = 'Stop'
# Cache misses are expected; check each native command explicitly.
$PSNativeCommandUseErrorActionPreference = $false
if (-not $env:OST_HOME -or -not $env:OST_RUNTIME_ARTIFACT -or -not $env:OST_RUNTIME_REMOTE) {
  throw 'OST_HOME, OST_RUNTIME_ARTIFACT and OST_RUNTIME_REMOTE are required'
}
$workspace = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$evidence = Join-Path $workspace '.ost-ci'
New-Item -ItemType Directory -Force $evidence | Out-Null

ost artifact show $env:OST_RUNTIME_ARTIFACT --json *> $null
if ($LASTEXITCODE -eq 0) {
  ost artifact verify $env:OST_RUNTIME_ARTIFACT --json |
    Out-File -Encoding utf8 (Join-Path $evidence 'runtime-cache-verify.json')
  if ($LASTEXITCODE -ne 0) { throw 'cached OpenUSD runtime verification failed' }
} else {
  ost artifact pull $env:OST_RUNTIME_REMOTE `
    --expect-artifact $env:OST_RUNTIME_ARTIFACT `
    --require-kind runtime --require-openusd cy2026/windows/x86_64/vulkan `
    --require-openusd-version $Version --require-sbom --require-provenance --json |
    Tee-Object -FilePath (Join-Path $evidence 'runtime-pull.json')
  if ($LASTEXITCODE -ne 0) { throw 'OpenUSD runtime pull failed' }
}
ost runtime pull cy2026 --profile usd --from-artifact $env:OST_RUNTIME_ARTIFACT --force
if ($LASTEXITCODE -ne 0) { throw 'OpenUSD runtime materialization failed' }
# runtime validate probes host devices and renders usdrecord. Keep that evidence
# opt-in for capability jobs; artifact verification and Merlin's compile/package
# tests provide the hosted gate without any host/device execution.
if ($ValidateRuntime) {
  ost runtime validate cy2026 --profile usd --json |
    Tee-Object -FilePath (Join-Path $evidence 'runtime-validate.json')
  if ($LASTEXITCODE -ne 0) { throw 'OpenUSD runtime validation failed' }
}
$runtimeJson = ost runtime show cy2026 --profile usd --json
if ($LASTEXITCODE -ne 0) { throw 'OpenUSD runtime inspection failed' }
$runtime = ($runtimeJson | ConvertFrom-Json).data
$root = Join-Path $env:OST_HOME "runtimes/$($runtime.id)"
$header = Join-Path $root 'include/pxr/pxr.h'
$expectedPxrVersion = [int]($Version.Replace('.', ''))
if (-not (Test-Path -LiteralPath $header) -or
    (Get-Content -LiteralPath $header -Raw) -notmatch "(?m)^#define PXR_VERSION $expectedPxrVersion\s*$") {
  throw "materialized SDK does not contain OpenUSD $Version headers: $root"
}
# Check cached and newly pulled SDKs alike; require the native Hgi bridge SDK.
if (-not (Test-Path -LiteralPath (Join-Path $root 'include/pxr/imaging/hgiVulkan/hgi.h'))) {
  throw "materialized SDK is missing HgiVulkan headers: $root"
}
$env:OPENUSD_ROOT = $root
if ($env:GITHUB_ENV) {
  "OPENUSD_ROOT=$root" | Out-File -Append -Encoding utf8 $env:GITHUB_ENV
}
Write-Host "OpenUSD ${Version}: $root"
