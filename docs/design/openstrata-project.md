# OpenStrata project layout

hdMerlin is adopted as one OpenStrata renderer project targeting `cy2026`.
The repository is not an OpenStrata workspace and its internal libraries are
not modeled as independently versioned OpenStrata packages. The installed
`Merlin` CMake package remains the distribution boundary.

## Composition mapping

| Renderer role | Project-owned CMake target |
| --- | --- |
| Host-neutral core | `merlin-render-world` |
| Extraction seam | `merlin-render-extraction` |
| Vulkan backend | `merlin-vulkan` |
| Headless adapter | `merlin-headless` |
| Hydra 2 adapter | `hdMerlin` |

`openstrata.toml` selects the host-neutral `core` runtime profile. Vulkan is a
project build capability supplied by the host, while Hydra development selects
a real `usd` or `lookdev` runtime explicitly. This keeps normal OST builds free
of an implicit OpenUSD dependency without changing the existing optional CMake
layers.

## CLI and runtime policy

OST 0.19.0 or newer is the operational baseline. It supplies atomic completion
records for managed builds, explicit generator/timeout controls, strict runtime
fingerprinting, and the managed `renderer view` lifecycle used by this project.
Confirm the selected executable before diagnosing project behavior:

```console
ost --version
ost runtime list --json
```

The committed `openstrata.toml` default remains `cy2026` / `core`. That profile
is the normal host-neutral build and validation path:

```console
ost runtime pull cy2026 --profile core
ost build --check
ost build --jobs auto
ost validate --json
```

`ost build` uses the existing CMake graph through a tool-owned
`CMakeUserPresets.json` entry and writes atomic completion evidence only after
configure, build, and output verification succeed. Ninja is the default;
`--generator`, `--configure-timeout`, and `--build-timeout` are supported escape
hatches. A Vulkan build writes `renderer-report.json` at the target build root.
Unavailable GPU or validation-layer capability is an explained skip; an
exercised renderer contract failure remains a build failure.

`ost validate` without `--build-dir` validates the OST-managed tree and its
completion/runtime fingerprint. Use `ost validate --build-dir <dir>` only for a
manual/external CMake tree; the result intentionally does not claim that `ost
build` configured or built it.

## Managed Hydra view (normal path)

`renderer view` requires a compatible real OpenUSD imaging/usdview runtime.
Create one explicitly by adopting an existing install, building OpenUSD, or
materializing a verified runtime artifact. A bare pull with only `--profile usd`
may create a mock layout and is not sufficient for interactive Hydra use.

```powershell
# Choose one real-runtime source:
ost runtime pull cy2026 --profile usd --from-usd C:/path/to/openusd
# ost runtime pull cy2026 --profile usd --build C:/path/to/OpenUSD
# ost runtime pull cy2026 --profile usd --from-artifact sha256:<digest>

ost runtime validate cy2026 --profile usd --json
ost renderer view --profile usd
```

With no `--build-dir`, `ost renderer view [scene]` is the normal managed loop.
It resolves a unique compatible real runtime, requests the shared `hydra2` build
intent, incrementally configures/builds a fingerprinted tree, stages the install
under `.strata/renderer-view`, discovers the installed `plugInfo.json`, selects
`Merlin`, and launches usdview in an isolated runtime environment. Omit the scene
to use the installed smoke scene; pass a project-relative or absolute USD path
for normal inspection.

```powershell
ost renderer view adapters/merlin-hydra2/tests/usdview-smoke.usda `
  --profile usd --config Release
```

`ost env` is not required for this managed path, and the command does not mutate
the parent shell.

## External/prebuilt Hydra view

Use this path when a Visual Studio or otherwise project-owned build tree already
exists. Resolve the same real runtime for CMake and the host session, configure
and build it yourself, then identify that tree explicitly:

```powershell
$ostUsd = (ost env cy2026 --profile usd --json | ConvertFrom-Json).data.prefix

cmake -S . -B out-hydra -G "Visual Studio 17 2022" -A x64 `
  -DMERLIN_ENABLE_HYDRA2=ON `
  "-DCMAKE_PREFIX_PATH=$ostUsd"
cmake --build out-hydra --config Release --parallel 4

ost validate --profile usd --build-dir out-hydra --json
ost renderer view adapters/merlin-hydra2/tests/usdview-smoke.usda `
  --build-dir out-hydra --profile usd --config Release
```

In this mode OST validates, installs, and inspects the external build, but it
does not reconfigure/rebuild it or create an OST-managed completion claim.
Runtime fingerprint mismatches fail early. The co-built Hydra adapter remains a
renderer runtime product rather than a standalone OpenStrata plugin bundle in
both modes.

## CI policy

Capability CI bootstraps the exact OST version declared in the workflow and
checksum-verifies its release asset. It materializes the digest-pinned OpenUSD
runtime artifact, validates the runtime, runs normal renderer lifecycle checks,
and retains runtime/build/render evidence. Local developer installations may be
newer, but CLI behavior used by CI must be reproduced with the pinned version
before changing operational claims.

## Adoption provenance

This repository predates the renderer scaffold and supplied much of the
dogfood evidence for its boundaries. It therefore commits the renderer domain
manifest but does not claim `openstrata.scaffold.yaml` provenance: the existing
implementation was adopted, not generated by `ost init --template renderer`.
Future template changes are reviewed as explicit migrations rather than
regenerating project-owned source. OST 0.19.0 provides `ost renderer adopt` for
new existing-renderer adoptions; this repository's already-reviewed manifest is
not regenerated on routine CLI upgrades.
