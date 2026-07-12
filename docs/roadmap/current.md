# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in [release records](../releases/).

## Next milestone: v0.2.0 — resource-granular GPU scene

**Status:** implemented on `v0.2.0-resource-granular-gpu-scene`; exit criteria
are enforced by the `merlin-vulkan-resource-update` and `merlin-benchmark-json`
CTest gates. Release pending.

Define an immutable `FrameSnapshot`; split mesh geometry, material, instance,
and draw records; key extraction and GPU caches by handle, generation, and
revision; implement transform-, visibility-, and material-only updates; share
geometry across instances; validate removal across frame latency; and add a
device-local staging ring, dirty-range transfer, and buffer suballocation.

### Exit criteria

- Static scenes produce zero upload bytes and zero pipeline creation after
  warm-up.
- Transform-, visibility-, and material-only edits avoid rebuilding unrelated
  geometry resources.
- Removed resources remain valid across configured frame latency and are retired
  deterministically.
- Benchmarks report upload bytes, allocation churn, and pipeline/cache activity
  for first-frame, steady-state, and edit scenarios.

## Carry-over follow-ups

- Define a host-neutral diagnostic sink instead of writing directly to stderr or
  a DCC-specific logger.
- Add configure-time OpenUSD version, build-configuration, and C++ runtime ABI
  compatibility checks.
- Define a common machine-readable result schema for discovery, delegate
  creation, RenderBuffer, GPU render, and host-presentation assertions.
