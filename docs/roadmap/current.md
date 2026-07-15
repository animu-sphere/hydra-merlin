# Current

Only incomplete work for the next release milestone and active carry-over is
listed here. Completed pre-release detail is retained in the
[delivery history](../reports/delivery-history.md); shipped versions will be
recorded in the [changelog](../../CHANGELOG.md).

## Next milestone

### 🚧 v0.7.0 — Persistent RenderWorld and GPU residency

**Objective:** make static and localized Mesh/Gaussian scene work scale with
changed resources rather than total prim count while keeping resource identity
stable across frames.

v0.6.0 established locator-aware Hydra ingestion, resource-granular revisions,
changed ranges, and safe partial upload. The remaining v0.7.0 work completes
persistent snapshot construction, bindless resource identity, and measured GPU
residency on top of those contracts.

#### 1. Persistent snapshot completion

- Eliminate the structurally reported full-table/draw fallback for resource
  additions and removals while retaining dense transient draw indices and
  immutable older snapshots.
- Keep texture/sampler structural binding invalidation proportional to
  dependent materials instead of conservatively copying every material.
- Extend localized-edit evidence from the current 10,000-record fixture to 100
  edits in a one-million-prim scene, including additions and removals.

#### 2. Descriptor-indexing negotiation

- Probe every descriptor-indexing feature and sampled-image/sampler limit used
  by the backend, enable only the selected feature chain, and report the
  versioned capability result.
- Select bindless or conventional Forward explicitly, with a machine-readable
  fallback reason and coverage for feature, limit, and configuration failures.

#### 3. Bindless texture and sampler tables

- Add finite generation-checked texture and sampler slot allocators with stale
  generation detection, capacity/exhaustion diagnostics, and current/peak use
  telemetry.
- Reserve white, black, flat-normal, and error texture slots; deduplicate
  samplers by descriptor value; and keep indices stable for unchanged
  resources.
- Update only dirty descriptor slots and retire replaced textures, samplers,
  and slot generations only after their last completion value.
- Add a non-uniform-indexed bindless Forward shader path while retaining the
  conventional descriptor implementation as the correctness fallback.

#### 4. Residency, transfer, and memory budget

- Complete persistent arena and mapped-upload-ring telemetry for stable ranges,
  growth, fragmentation, retirement, and bytes staged by resource class.
- Add asynchronous transfer-queue selection, ownership transitions, and
  timeline synchronization without weakening in-flight replacement safety.
- Probe heap budget/usage, define configurable VRAM limits and exhaustion
  behavior, and retain current/peak/capacity evidence in capability and
  benchmark artifacts.

#### 5. Validation and release evidence

- Cover slot allocation/retirement/reuse, generation mismatch, fallback slots,
  sampler deduplication, exhaustion, non-uniform indexing, partially-bound
  descriptors, and in-flight replacement.
- Retain conventional/bindless image parity, one-million-prim localized-edit
  scaling, steady-state zero-work, fallback selection, and VRAM evidence.

#### Scope boundary

v0.7.0 owns the common arena, upload, retirement, descriptor, and memory-budget
infrastructure needed by Mesh and future Gaussian resources. Host-neutral
`GaussianResource`, standard Hydra Gaussian ingestion, and native Gaussian
rendering remain v0.9.0 work; this milestone does not introduce a renderer-
specific Gaussian schema or file parser.

#### Exit criteria

- Static snapshot cost no longer scales with total prim count, and changing 100
  resources in a one-million-prim scene scales approximately with those
  changes.
- Unchanged GPU resource addresses, handles, and descriptor indices remain
  stable; stale generations are detectable and texture replacement is safe
  across frames in flight.
- Bindless Forward matches the reference image, while descriptor work scales
  with changed resources rather than materials or draws.
- Steady state performs zero upload, allocation, descriptor allocation/update,
  shader compilation, pipeline creation, and CPU wait for GPU.

## Active carry-over

- Enroll or confirm a repository- or organization-scoped Windows x64 runner
  with the existing `vulkan-1.4` label so the manual workflow becomes
  continuing GPU evidence.
- Retain dependency/runtime provenance, validation logs, expected/actual/diff
  images, benchmark output, Hydra discovery, RenderBuffer, and usdview results
  as comparable artifacts rather than reducing capability jobs to a binary
  pass/fail signal.
