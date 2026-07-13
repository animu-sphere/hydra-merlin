# Releasing

`VERSION` is the release source of truth for the project, package, installed
metadata, and release tag version. The release helper synchronizes the required
OpenStrata project version in `openstrata.toml`. Normal feature work only adds
notes under `[Unreleased]` in `CHANGELOG.md`; it does not update version
references in CMake, README, roadmap, or support documents.

## Prepare and publish

From a release branch, run the helper with the version being prepared. This
example shows v0.5.0:

```powershell
./scripts/prepare-release.ps1 -Version 0.5.0
```

The command performs the mechanical release edits:

- writes `0.5.0` to `VERSION`;
- synchronizes `openstrata.toml` to `0.5.0`;
- moves the existing Unreleased notes under a dated `0.5.0` heading;
- updates the Unreleased comparison base; and
- adds the stable release comparison link.

Use `-Date YYYY-MM-DD` to override the UTC date or `-DryRun` to validate without
writing. The platform-neutral equivalent is:

```console
cmake -DMERLIN_RELEASE_VERSION=0.5.0 -P cmake/prepare-release.cmake
```

Review the three changed files, commit them, merge to `main`, and wait for hosted
CI. Then create the matching immutable tag:

```console
git tag -a v0.5.0 -m "hdMerlin v0.5.0"
git push origin v0.5.0
```

README, roadmap, support-matrix, and detailed release-record edits are required
only when their content actually changes; they are not release bookkeeping.

Release review must also confirm that the support matrix describes at least the
release being tagged, `current.md` does not list shipped work as incomplete,
`backlog.md` does not retain a shipped milestone, and README capability
boundaries agree with the release record. These content checks are deliberate
review items even though identity checks are automated.

## Automated contract

The tag-driven `Release` workflow rejects a tag unless all of these agree:

- the tag uses exactly `vMAJOR.MINOR.PATCH`;
- `VERSION` contains the same `MAJOR.MINOR.PATCH`; and
- `openstrata.toml` contains the same project version; and
- `CHANGELOG.md` contains the dated stable section and comparison link.

It then performs clean Windows and Linux Core-only Release builds, runs source
and isolated install-tree tests, installs each SDK into a fresh staging prefix,
and publishes archives, metadata sidecars, and SHA-256 checksum files to the
tag's GitHub Release. GitHub-generated notes supplement the canonical changelog.

Each SDK contains `merlin-release-metadata.json` under its data directory. The
file records schema and project versions, dependency constraints, configured
feature layers, exported targets, and runtime-only products.

The publish job only runs on a real tag push, so watch the first tag after a
pipeline change closely. Release tags are immutable: fix a published workflow
or package issue in a new patch version rather than moving an existing tag.

## Runtime products

The hosted workflow publishes the portable Core SDK baseline. Vulkan, headless,
benchmark, and Hydra products remain capability-dependent source-build products.
The manually dispatched GPU capability workflow exercises Vulkan/headless in
Debug and Release and Hydra in Release on a runner labeled `vulkan-1.4`.
