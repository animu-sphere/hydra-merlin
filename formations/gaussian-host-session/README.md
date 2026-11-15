# Gaussian PLY host session

This OpenStrata Formation opens a Graphdeco-style PLY directly in `testusdview`
and renders it with Merlin. The smoke test checks the imported
`/Asset/Splat` prim and captures the three colored splats to `gaussian-ply.png`.
The fixture is synthetic and ships with this repository.

The preview targets Windows x86_64 with Python 3.13, OpenUSD 26.08 Lookdev,
and a Vulkan 1.4 device. The OpenGL OpenUSD host uses Merlin's Tier 0 CPU
RenderBuffer presentation path and the default CPU-sorted Gaussian path.

## Prepare the artifacts

Use OpenStrata 0.23.20 and pull the runtime pinned in the manifest:

```powershell
ost artifact pull oci://ghcr.io/animu-sphere/openstrata-runtime-cy2026-lookdev@sha256:b840ed4690aa39d4582bc03c0a09fa7bab717216635b55a4eb718482dbfb196b --expect-artifact sha256:b982656c07dd9147973e3c7197d9b050ee48dd1ac291988f7e6025c76c4a785b --require-kind runtime
ost runtime pull cy2026 --profile lookdev --from-artifact sha256:b982656c07dd9147973e3c7197d9b050ee48dd1ac291988f7e6025c76c4a785b
```

Both the `gaussian-ply` plugin and `hdMerlin` renderer packages must target
that exact runtime. The published USD 3DGS Plugins v0.8.0 product targets the
`usd` profile. Build a `lookdev` package from its release source for local
verification. Run these commands in the `usd-3dgs-plugins` checkout at `v0.8.0`:

```powershell
ost plugin build plugins/gaussian-ply --profile lookdev
ost plugin package plugins/gaussian-ply --profile lookdev
```

In the Merlin checkout run:

```powershell
ost build --profile lookdev --intent hydra-usd --jobs auto
ost package --profile lookdev --intent hydra-usd
```

Run `ost artifact import <package-dist-directory>` for each package. Use the
full SHA-256 artifact digests reported by the imports. See the
[build guide](../../docs/guides/build-and-install.md) for Vulkan and Slang
prerequisites.

## Lock and run

Run from `formations/gaussian-host-session`, where the test and fixture paths
resolve. Set `$gaussianPlyDigest` and `$merlinDigest` to the respective
`sha256:...` artifact digests before running these commands in PowerShell 7:

```powershell
(Get-Content -Raw formation.toml.in).Replace('@GAUSSIAN_PLY_ARTIFACT@', $gaussianPlyDigest).Replace('@MERLIN_ARTIFACT@', $merlinDigest) |
    Set-Content -Encoding utf8NoBOM formation.toml
ost formation lock formation.toml
ost formation doctor formation.toml
ost formation run formation.toml
```

To view the same asset interactively, use `ost formation run formation.toml
-- usdview sample.ply --renderer Merlin`.

The public preview requires publishing both matching packages and checking in
the resulting `formation.toml` and `formation.lock` with the artifact pull
instructions. Verify the locked session in a fresh artifact store before
claiming the published workflow.
