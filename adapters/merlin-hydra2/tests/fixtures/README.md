# Gaussian test fixtures

`leica-sofort-top8192.usdc` is a flattened OpenUSD crate containing one
standard `ParticleField3DGaussianSplat` with 8,192 particles and degree-3
spherical-harmonic appearance. It is the repository-owned integration corpus
for Gaussian schema ingestion, usdview/Hydra rendering, reference-image, and
performance evidence.

The asset was created by the project maintainer and is dedicated to the public
domain under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).
Its SHA-256 digest is
`0c08344de31d159c82a2011ef3716c86cb93932c36ef70e44c7a2cbc54661ca4`.

`leica-sofort-top8192-reference.png` is the 597×540 Tier 0 usdview capture
generated from that stage with hdMerlin's v0.14.1 CPU-sorted Vulkan reference
path. Its SHA-256 digest is
`af70da6de81fb9f8fc8fba09fd6e3a0680be0b51b736f6766d43a33bc0a227f6`.
The host smoke compares later captures against it with a small
cross-driver tolerance rather than treating PNG byte identity as the rendering
contract.

`leica-sofort-top8192-reference-vulkan-float.png` is the 597×540 Tier 0
CPU-sorted Vulkan capture from 2026-10-04, with RGBA32F accumulation, one final
RGBA8 conversion and pixel-center ellipse evaluation. It was captured on an
RTX A5000 with driver 597.16, Vulkan SDK 1.4.350.0 and OpenUSD 26.08; HgiVulkan
color copy produced the same PNG bytes. Its SHA-256 digest is
`dec8deec02136c53ab0e89624bb31ba89cd6ce234bd1263c204a953731f8eb73`.
It is the Vulkan float-composite host baseline. The earlier reference remains
available for the legacy UNorm path and Metal; host tolerances are unchanged.
