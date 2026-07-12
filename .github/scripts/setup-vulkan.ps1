$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$version = "1.4.350.0"
$expectedSha256 = "855b27ba05d2d8119c5114c5d4ff870ca38f2c632b11e1bb9923b9b7e6ecfe7b"
$workspace = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$sdkRoot = Join-Path $workspace ".ci/vulkan-sdk/$version"
$sdkBin = Join-Path $sdkRoot "Bin"
$glslc = Join-Path $sdkBin "glslc.exe"

# Tools the capability jobs rely on. A restored cache is only trusted when all
# of them are present, so a partially populated prefix triggers a reinstall.
$requiredTools = @("glslc.exe", "vulkaninfo.exe")
function Get-MissingVulkanTool {
  foreach ($tool in $requiredTools) {
    if (-not (Test-Path -LiteralPath (Join-Path $sdkBin $tool))) {
      return $tool
    }
  }
  return $null
}

if (Get-MissingVulkanTool) {
  $downloads = Join-Path $workspace ".ci/downloads"
  $installer = Join-Path $downloads "vulkan-sdk-$version.exe"
  New-Item -ItemType Directory -Force $downloads | Out-Null
  Invoke-WebRequest "https://sdk.lunarg.com/sdk/download/$version/windows/vulkan_sdk.exe" -OutFile $installer
  $actualSha256 = (Get-FileHash -Algorithm SHA256 $installer).Hash.ToLowerInvariant()
  if ($actualSha256 -ne $expectedSha256) {
    throw "LunarG Vulkan SDK $version hashes to $actualSha256, expected $expectedSha256"
  }
  if (Test-Path -LiteralPath $sdkRoot) {
    Remove-Item -LiteralPath $sdkRoot -Recurse -Force
  }
  & $installer --root $sdkRoot --accept-licenses --default-answer `
    --confirm-command install copy_only=1
  if ($LASTEXITCODE -ne 0) {
    throw "LunarG Vulkan SDK installer exited with $LASTEXITCODE"
  }
}

$missingTool = Get-MissingVulkanTool
if ($missingTool) {
  throw "LunarG Vulkan SDK installation is missing $(Join-Path $sdkBin $missingTool)"
}

$env:VULKAN_SDK = $sdkRoot
$env:VK_LAYER_PATH = $sdkBin
$env:PATH = "$sdkBin;$env:PATH"

if ($env:GITHUB_ENV) {
  "VULKAN_SDK=$sdkRoot" | Out-File -Append -Encoding utf8 $env:GITHUB_ENV
  "VK_LAYER_PATH=$sdkBin" | Out-File -Append -Encoding utf8 $env:GITHUB_ENV
}
if ($env:GITHUB_PATH) {
  $sdkBin | Out-File -Append -Encoding utf8 $env:GITHUB_PATH
}

Write-Host "LunarG Vulkan SDK ${version}: $sdkRoot"
& $glslc --version
