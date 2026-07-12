# Releasing

hdMerlin releases are initiated only by pushing a stable SemVer tag. There is no
manual-dispatch release path.

## Release contract

1. Land the intended release commit on `main` with hosted CI passing.
2. Confirm that `project(hdMerlin VERSION ...)` and the changelog describe the
   intended version.
3. Create and push the matching tag, for example `v0.1.0`.

The `Release` workflow rejects tags that are not exactly
`vMAJOR.MINOR.PATCH`, or whose version differs from the CMake project version.
It then performs clean Windows and Linux Core-only Release builds, runs source
and isolated install-tree tests, installs each SDK into a fresh staging prefix,
and publishes the archives, metadata sidecars, and SHA-256 checksum files to the
tag's GitHub Release.

Each SDK contains `merlin-release-metadata.json` under its data directory. The
file records schema and project versions, dependency constraints, configured
feature layers, exported targets, and runtime-only products.

The `publish` job (artifact upload, download, and asset creation) only runs on a
real tag push, so it is not exercised by hosted pull-request CI. Watch the first
tag of a new pipeline closely, and prefer validating pipeline changes on a
throwaway fork tag before tagging the canonical repository.

## Runtime products

The hosted release workflow publishes the portable Core SDK baseline. Vulkan,
headless, benchmark, and Hydra products remain capability-dependent source-build
products in v0.1.0. The manually dispatched GPU capability workflow exercises
Vulkan/headless in Debug and Release and Hydra in Release on a runner labeled
`vulkan-1.4`.

Release tags are immutable. If a workflow or package issue is found after a tag
is published, fix it in a new patch version instead of moving the existing tag.
