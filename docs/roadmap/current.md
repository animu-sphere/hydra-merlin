# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone: v0.4.0 release verification

The execution and render-product lifetime implementation is complete on the
v0.4.0 feature branch. Remaining release work is evidence and finalization:

- 🚧 Run the Vulkan capability workflow in Debug and Release and retain the
  request/submit/resolve, validation, and PNG/EXR comparison artifacts.
- ⬜ Finalize the changelog date and version metadata with `prepare-release`
  only after the capability evidence is accepted.

## Carry-over follow-ups

- Define a host-neutral diagnostic sink instead of writing directly to stderr or
  a DCC-specific logger.
- Add configure-time OpenUSD version, build-configuration, and C++ runtime ABI
  compatibility checks.
- Merge the optional Hydra discovery, delegate, RenderBuffer, and usdview
  results into the root OpenStrata renderer report instead of leaving those
  assertions as explained skips in the headless evidence path.
