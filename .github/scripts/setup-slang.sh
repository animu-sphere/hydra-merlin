#!/usr/bin/env bash
set -euo pipefail

# Keep the complete distribution: slangc loads libraries beside the binary.
case "$(uname -s)/$(uname -m)" in
  Darwin/arm64)
    slang_platform=macos-aarch64
    slang_sha=13949e0fc81f5c0bcde9a195190706740f1cc77af9c558f9f1b598e9bea3c606 ;;
  Darwin/x86_64)
    slang_platform=macos-x86_64
    slang_sha=ff143c6fb6d6aa08545c1f4c68f14df06a92d52bccd5ad9cf2810ccd9cd61bfa ;;
  *) echo "Unsupported Slang CI platform" >&2; exit 1 ;;
esac
slang_root="${RUNNER_TEMP:?}/merlin-slang-2026.8"
slang_archive="${RUNNER_TEMP}/merlin-slang.tar.gz"
curl --fail --location --retry 3 \
  "https://github.com/shader-slang/slang/releases/download/v2026.8/slang-2026.8-${slang_platform}.tar.gz" \
  -o "$slang_archive"
echo "$slang_sha  $slang_archive" | shasum -a 256 --check
mkdir -p "$slang_root"
tar -xzf "$slang_archive" -C "$slang_root"
"$slang_root/bin/slangc" -version
echo "$slang_root/bin" >> "${GITHUB_PATH:?}"
