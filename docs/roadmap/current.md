# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in [release records](../releases/).

## Next milestone: v0.1.0 — reproducible renderer foundation

**Status:** release candidate · **Depends on:** hosted CI passing on the release
commit and the matching `v0.1.0` tag being pushed.

The engineering exit criteria are implemented: clean configuration coverage,
the runtime-only Headless/Hydra packaging contract, tag-driven release
automation, and versioned machine-readable package metadata.

### Remaining release gate

- Merge the release-candidate changes after hosted Core CI passes.
- Push the immutable `v0.1.0` tag. The tag workflow must reproduce the hosted
  Core SDKs and publish their metadata and checksums before the release is
  recorded in [release records](../releases/).

## Carry-over follow-ups

- Define a host-neutral diagnostic sink instead of writing directly to stderr or
  a DCC-specific logger.
- Add configure-time OpenUSD version, build-configuration, and C++ runtime ABI
  compatibility checks.
- Define a common machine-readable result schema for discovery, delegate
  creation, RenderBuffer, GPU render, and host-presentation assertions.
