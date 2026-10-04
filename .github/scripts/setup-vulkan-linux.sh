#!/usr/bin/env bash
# Source this script so the pinned SDK remains visible to subsequent commands.
set -euo pipefail

workspace="${GITHUB_WORKSPACE:-$(pwd)}"
sdk_version=1.4.350.1
sdk_sha=6cce33c7e5383814150c5041820769d93c65a1fd883002e5949b067045a07daa
slang_sha=b23af8f2569e7961ca143c6ecd9abf6d7ac14a9bda739a1c6281694f774fa3eb
tools_root="$workspace/.ci/linux-vulkan"
downloads="$workspace/.ci/downloads"
mkdir -p "$tools_root" "$downloads"

fetch_archive() {
  local url="$1" archive="$2" sha="$3" destination="$4"
  if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 "$url" -o "$archive"
  fi
  echo "$sha  $archive" | sha256sum --check
  mkdir -p "$destination"
  tar -xf "$archive" -C "$destination"
}

if [[ ! -x "$tools_root/$sdk_version/x86_64/bin/spirv-val" ]]; then
  fetch_archive \
    "https://sdk.lunarg.com/sdk/download/$sdk_version/linux/vulkan_sdk.tar.xz" \
    "$downloads/vulkan-sdk-$sdk_version.tar.xz" "$sdk_sha" "$tools_root"
fi
if [[ ! -x "$tools_root/slang-2026.8/bin/slangc" ]]; then
  fetch_archive \
    https://github.com/shader-slang/slang/releases/download/v2026.8/slang-2026.8-linux-x86_64.tar.gz \
    "$downloads/slang-2026.8-linux-x86_64.tar.gz" "$slang_sha" \
    "$tools_root/slang-2026.8"
fi

export VULKAN_SDK="$tools_root/$sdk_version/x86_64"
export PATH="$tools_root/slang-2026.8/bin:$VULKAN_SDK/bin:$PATH"
export LD_LIBRARY_PATH="$VULKAN_SDK/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export VK_LAYER_PATH="$VULKAN_SDK/share/vulkan/explicit_layer.d"
# Mesa installations use both unqualified and architecture-qualified names.
mapfile -t lavapipe_icds < <(find /usr/share/vulkan/icd.d -maxdepth 1 -name 'lvp_icd*.json' | sort)
if [[ ${#lavapipe_icds[@]} != 1 ]]; then
  echo "Expected exactly one Mesa lavapipe ICD, found ${#lavapipe_icds[@]}" >&2
  return 1
fi
export VK_DRIVER_FILES="${lavapipe_icds[0]}"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
slangc -version

if [[ -n "${GITHUB_ENV:-}" ]]; then
  for variable in VULKAN_SDK LD_LIBRARY_PATH VK_LAYER_PATH VK_DRIVER_FILES VK_ICD_FILENAMES; do
    echo "$variable=${!variable}" >> "$GITHUB_ENV"
  done
  printf '%s\n' "$tools_root/slang-2026.8/bin" "$VULKAN_SDK/bin" >> "$GITHUB_PATH"
fi
