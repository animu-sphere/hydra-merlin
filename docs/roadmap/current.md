# Current

Active and immediately next work lives here. Shipped changes are recorded in
the [changelog](../../CHANGELOG.md) and [v0.16.1 release record](../releases/v0.16.1.md);
current support claims live in the [support matrix](../reference/support-matrix.md).

## Next — validation gates

- [ ] Record the first hosted results of the new `Hydra compilation CI` workflow
  for both pinned OpenUSD 26.05/26.08 Release SDKs. The independent Windows gate
  now builds Hydra/HgiVulkan and checks CPU contracts, shader/ABI, package linking
  and installed plugin discovery. Verify both jobs before claiming hosted Hydra
  coverage in the support matrix; device/host execution and scheduled Metal
  capability evidence remain separate, without universal GPU timing gates.

Historical producer-session evidence and remaining OpenStrata integration asks
are in the [OST recheck](../reports/ost/13-2026-09-26-v0.23.8-recheck-v0.24.0-asks.md).
