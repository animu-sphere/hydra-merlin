$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$version = "1.4.350.0"
$expectedSha256 = "855b27ba05d2d8119c5114c5d4ff870ca38f2c632b11e1bb9923b9b7e6ecfe7b"
$workspace = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$sdkRoot = Join-Path $workspace ".ci/vulkan-sdk/$version"
$sdkBin = Join-Path $sdkRoot "Bin"
$slangc = Join-Path $sdkBin "slangc.exe"

# Files the compilation and capability jobs rely on. HgiVulkan's public wrapper
# includes the SDK-provided vma/vk_mem_alloc.h, so an executable-only cache is
# not a complete development SDK.
$requiredFiles = @(
  "Bin/slangc.exe",
  "Bin/vulkaninfoSDK.exe",
  "Include/vulkan/vulkan.h",
  "Lib/vulkan-1.lib",
  "Lib/shaderc_combined.lib"
)
function Get-MissingVulkanFile {
  foreach ($file in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot $file))) {
      return $file
    }
  }
  return $null
}

if (Get-MissingVulkanFile) {
  $downloads = Join-Path $workspace ".ci/downloads"
  $installer = Join-Path $downloads "vulkan-sdk-$version.exe"
  New-Item -ItemType Directory -Force $downloads | Out-Null
  Invoke-WebRequest "https://sdk.lunarg.com/sdk/download/$version/windows/vulkan_sdk.exe" -OutFile $installer
  $actualSha256 = (Get-FileHash -Algorithm SHA256 $installer).Hash.ToLowerInvariant()
  if ($actualSha256 -ne $expectedSha256) {
    throw "LunarG Vulkan SDK $version hashes to $actualSha256, expected $expectedSha256"
  }
  if (Test-Path -LiteralPath $sdkRoot) {
    $resolvedSdkRoot = [IO.Path]::GetFullPath($sdkRoot)
    $allowedRoot = [IO.Path]::GetFullPath((Join-Path $workspace '.ci/vulkan-sdk')) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedSdkRoot.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
      throw "SDK replacement path is outside the workspace tool directory: $resolvedSdkRoot"
    }
    Remove-Item -LiteralPath $sdkRoot -Recurse -Force
  }
  & $installer --root $sdkRoot --accept-licenses --default-answer `
    --confirm-command install copy_only=1
  if ($LASTEXITCODE -ne 0) {
    throw "LunarG Vulkan SDK installer exited with $LASTEXITCODE"
  }
}

# LunarG's copy-only component omits VMA. Fetch the exact upstream header used
# by this SDK so a hosted runner needs no preinstalled self-host toolchain.
$vmaRelativePath = "Include/vma/vk_mem_alloc.h"
$vmaPath = Join-Path $sdkRoot $vmaRelativePath
$vmaSha256 = '90ce12fc4a2466235a09ae02905dd0c13aee80c1bbf11b331ab61230c2ceb112'
if (-not (Test-Path -LiteralPath $vmaPath)) {
  New-Item -ItemType Directory -Force (Split-Path $vmaPath) | Out-Null
  Invoke-WebRequest 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.3.0/include/vk_mem_alloc.h' -OutFile $vmaPath
}
if ((Get-FileHash -Algorithm SHA256 $vmaPath).Hash.ToLowerInvariant() -ne $vmaSha256) {
  throw "VMA 3.3.0 header checksum mismatch: $vmaPath"
}

$missingFile = Get-MissingVulkanFile
if ($missingFile) {
  throw "LunarG Vulkan SDK installation is missing $(Join-Path $sdkRoot $missingFile)"
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
& $slangc -version
