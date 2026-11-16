# Gaussian PLY host session

This OpenStrata Formation opens a Graphdeco-style PLY directly in `testusdview`
and renders it with Merlin. The smoke test checks the imported
`/Asset/Splat` prim and captures the three colored splats to `gaussian-ply.png`.
The fixture is synthetic and ships with this repository.

The preview targets Windows x86_64 with Python 3.13, OpenUSD 26.08 Lookdev,
and a Vulkan 1.4 device. The OpenGL OpenUSD host uses Merlin's Tier 0 CPU
RenderBuffer presentation path and the default CPU-sorted Gaussian path.

## Run the locked preview

Use OpenStrata 0.23.20 and run from `formations/gaussian-host-session`.
To reproduce the clean-store check, set `OST_HOME` to a short, empty directory.
Pull the OpenGL Lookdev runtime, `gaussian-ply` 0.8.0, and hdMerlin 0.16.6
from GHCR by OCI digest. Each pull verifies the artifact digest used by the
checked-in `formation.toml` and `formation.lock`.

```powershell
ost artifact pull oci://ghcr.io/animu-sphere/openstrata-runtime-cy2026-lookdev@sha256:b840ed4690aa39d4582bc03c0a09fa7bab717216635b55a4eb718482dbfb196b --expect-artifact sha256:b982656c07dd9147973e3c7197d9b050ee48dd1ac291988f7e6025c76c4a785b --require-kind runtime
ost artifact pull oci://ghcr.io/animu-sphere/usd-3dgs-plugins@sha256:c0e421a1b23d700fff2cc566e02f090b309fab8686204f4125aaa3e8010a02d8 --expect-artifact sha256:26ab97f2ef3516df71123511e2791ce817ee3b4d41595cf8ab81bc05b1a91d88 --require-kind plugin --require-sbom
ost artifact pull oci://ghcr.io/animu-sphere/hydra-merlin@sha256:0d04082d4de69684e2ea6011129b44580f68d7d6685e089e532f4455c97ecca9 --expect-artifact sha256:f80faafc45a6dd0b608815edaa3056479c03c3b578029f2430093ae4d2ca8527 --require-kind package --require-sbom
ost formation doctor formation.toml
ost formation run formation.toml
```

The smoke test checks the PLY root layer, `/Asset/Splat`, three Gaussian
points, and the active Merlin delegate. It writes `gaussian-ply.png` and
requires visible red, green, and blue splats. To open the fixture interactively:

```powershell
ost formation run formation.toml -- usdview sample.ply --renderer Merlin
```

## Build from source

Materialize the same runtime after pulling its artifact as shown above.

```powershell
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

### Lock a locally built package pair

Copy `formation.toml.in`, `sample.ply`, and `gaussian_ply_check.py` into a
separate directory and run the following commands there in PowerShell 7.
Set `$gaussianPlyDigest` and `$merlinDigest` to the artifact digests printed
by `ost artifact import`.

```powershell
(Get-Content -Raw formation.toml.in).Replace('@GAUSSIAN_PLY_ARTIFACT@', $gaussianPlyDigest).Replace('@MERLIN_ARTIFACT@', $merlinDigest) |
    Set-Content -Encoding utf8NoBOM formation.toml
ost formation lock formation.toml
ost formation doctor formation.toml
ost formation run formation.toml
```
