param(
  [Parameter(Mandatory = $true)]
  [ValidatePattern('^oci://ghcr\.io/[a-z0-9._/-]+@sha256:[0-9a-f]{64}$')]
  [string]$Reference,

  [Parameter(Mandatory = $true)]
  [string]$OutputDirectory,

  [ValidateRange(1, 20)]
  [int]$Attempts = 8
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

if ($Reference -notmatch '^oci://(?<registry>ghcr\.io)/(?<repository>[a-z0-9._/-]+)@(?<digest>sha256:[0-9a-f]{64})$') {
  throw "unsupported GHCR OCI reference: $Reference"
}

$registry = $Matches.registry
$repository = $Matches.repository
$manifestDigest = $Matches.digest
$rawReference = $Reference.Substring('oci://'.Length)
$manifestPath = "$OutputDirectory.oci-manifest.json"
$resultPath = "$OutputDirectory.pull.json"

$oras = Get-Command oras -ErrorAction Stop
$curl = Get-Command curl.exe -ErrorAction Stop
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null

& $oras.Source manifest fetch --output $manifestPath $rawReference
if ($LASTEXITCODE -ne 0) {
  throw "failed to fetch OCI manifest $manifestDigest"
}

$actualManifestDigest = 'sha256:' +
  (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant()
if ($actualManifestDigest -ne $manifestDigest) {
  throw "OCI manifest hashes to $actualManifestDigest, expected $manifestDigest"
}

$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
if (-not $manifest.layers -or $manifest.layers.Count -eq 0) {
  throw "OCI manifest $manifestDigest has no layers"
}

function Get-GhcrToken {
  $scope = [Uri]::EscapeDataString("repository:${repository}:pull")
  $tokenUri = "https://$registry/token?service=$registry&scope=$scope"
  $response = Invoke-RestMethod -Uri $tokenUri
  $token = if ($response.token) { $response.token } else { $response.access_token }
  if (-not $token) {
    throw "GHCR returned no anonymous pull token for $repository"
  }
  return $token
}

function Test-CompleteLayer {
  param(
    [string]$Path,
    [long]$ExpectedSize,
    [string]$ExpectedDigest
  )

  if (-not (Test-Path -LiteralPath $Path)) {
    return $false
  }
  if ((Get-Item -LiteralPath $Path).Length -ne $ExpectedSize) {
    return $false
  }
  $actual = 'sha256:' +
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
  return $actual -eq $ExpectedDigest
}

$layerResults = @()
foreach ($layer in $manifest.layers) {
  $title = $layer.annotations.'org.opencontainers.image.title'
  if (-not $title -or [IO.Path]::GetFileName($title) -ne $title) {
    throw "OCI layer $($layer.digest) has an unsafe or missing title"
  }
  if ($layer.digest -notmatch '^sha256:[0-9a-f]{64}$') {
    throw "OCI layer '$title' has an unsupported digest: $($layer.digest)"
  }

  $expectedSize = [long]$layer.size
  $outputPath = Join-Path $OutputDirectory $title
  if ((Test-Path -LiteralPath $outputPath) -and
      (Get-Item -LiteralPath $outputPath).Length -gt $expectedSize) {
    Remove-Item -LiteralPath $outputPath -Force
  }

  $initialSize = if (Test-Path -LiteralPath $outputPath) {
    (Get-Item -LiteralPath $outputPath).Length
  } else {
    0
  }
  $completedAttempt = 0

  for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
    if (Test-CompleteLayer $outputPath $expectedSize $layer.digest) {
      $completedAttempt = $attempt
      break
    }

    if ((Test-Path -LiteralPath $outputPath) -and
        (Get-Item -LiteralPath $outputPath).Length -eq $expectedSize) {
      Write-Warning "$title reached its expected size with the wrong digest; restarting it"
      Remove-Item -LiteralPath $outputPath -Force
    }

    $offset = if (Test-Path -LiteralPath $outputPath) {
      (Get-Item -LiteralPath $outputPath).Length
    } else {
      0
    }
    Write-Host "Pulling $title (attempt $attempt/$Attempts, offset $offset/$expectedSize)"

    $registryToken = Get-GhcrToken
    $blobUri = "https://$registry/v2/$repository/blobs/$($layer.digest)"
    & $curl.Source --fail --silent --show-error --location `
      --connect-timeout 30 --speed-limit 1 --speed-time 120 `
      --continue-at - --header "Authorization: Bearer $registryToken" `
      --output $outputPath $blobUri
    $curlExit = $LASTEXITCODE

    if ($curlExit -eq 0 -and
        (Test-CompleteLayer $outputPath $expectedSize $layer.digest)) {
      $completedAttempt = $attempt
      break
    }
    if ($attempt -lt $Attempts) {
      $delay = [Math]::Min(30, [Math]::Pow(2, $attempt))
      Write-Warning "$title pull did not complete (curl exit $curlExit); retrying in $delay seconds"
      Start-Sleep -Seconds $delay
    }
  }

  if ($completedAttempt -eq 0) {
    $received = if (Test-Path -LiteralPath $outputPath) {
      (Get-Item -LiteralPath $outputPath).Length
    } else {
      0
    }
    throw "failed to pull $title after $Attempts attempts ($received/$expectedSize bytes)"
  }

  $layerResults += [ordered]@{
    title = $title
    digest = $layer.digest
    size = $expectedSize
    initial_size = $initialSize
    attempts = $completedAttempt
  }
}

[ordered]@{
  schema = 1
  reference = $Reference
  manifest_digest = $manifestDigest
  layers = $layerResults
} | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 $resultPath

Write-Host "Pulled and verified $($layerResults.Count) OCI layers from $manifestDigest"
