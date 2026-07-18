# OST dogfooding report — capability CI evidence contract

- Date: 2026-07-12
- Scope: Core-only hosted CI; Vulkan 1.4/headless and OpenUSD/Hydra capability
  CI; machine-readable Vulkan evidence
- Focus: reusable validation-runner and artifact boundaries for OST templates

## Conclusion

The hdMerlin CI split confirms that renderer validation should not be expressed
as one dependency-accumulating build matrix. Core portability is a normal hosted
CI concern. Vulkan validation and an interactive Hydra host are capability
contracts owned by explicitly labelled runners. Keeping those jobs separate
prevents missing GPUs, validation layers, UI support, or OpenUSD SDKs from
looking like renderer regressions.

The useful OST boundary is not a GitHub Actions file. It is a small evidence
contract: capability preflight, deterministic test selection, a versioned JSON
record of the selected runtime, and retained render/host artifacts even on
failure.

## Validated split

```text
hosted Windows/Linux
  -> Core-only Debug/Release
  -> source tests + install-tree consumer

self-hosted Windows Vulkan capability runner
  -> Vulkan 1.4 loader/device capability
  -> checksum-pinned LunarG SDK + validation layer
  -> headless 64-frame validation

self-hosted Windows Hydra capability runner
  -> Vulkan 1.4 + digest-pinned Animusphere OpenUSD runtime + testusdview
  -> install-tree usdview stable-update regression

both capability jobs
  -> JSON and/or images + host log + CTest log
```

The local Windows validation used Vulkan SDK/loader 1.4.350, an AMD device
reporting Vulkan 1.4.315, AMD driver 26.3.1, OpenUSD 26.05, and a Release Hydra
build. All nine tests passed, including the 64-frame validation loop and the
seven-phase usdview regression (baseline, points, topology, transform,
visibility, camera, and resize).

## OST template candidates

### Render-validation template

- Accept runner labels and dependency roots as template inputs; do not embed a
  vendor machine name or SDK installation path.
- Keep Vulkan/headless and Hydra jobs separate so OpenUSD is not a prerequisite
  for backend-only validation.
- Materialize OpenUSD from a digest-pinned runtime package so the GPU runner
  owns only its actual hardware/driver capability, not an SDK installation.
- Download the Vulkan SDK from LunarG's versioned endpoint, verify a pinned
  producer SHA-256, install with `copy_only=1`, and cache the resulting prefix.
- Separate capability-independent tests from GPU/host tests using CTest labels.
- Ensure Release test targets do not compile away standard `assert` checks;
  portability matrices must exercise the same assertions in every build type.
- Require the backend probe to write a versioned, machine-readable artifact.
- Retain the backend record, rendered images, structured regression log, and
  test-runner log with `always()` semantics.
- Keep the validation loop deterministic and structural: fixed dimensions and
  frame count, one scene upload for an unchanged scene, depth coverage, and zero
  validation warnings/errors.

### Vulkan-backend template

The minimum evidence payload proven here contains Vulkan SDK/header version,
loader API version, selected-device API version and identity, raw driver
version, driver name/info, timeline-semaphore support, and validation state.
Raw numeric vendor/device/driver values should be retained alongside readable
driver strings because Vulkan driver version encoding is vendor-specific.

The SDK version is build provenance, while loader/device/driver values are
runtime provenance. A reusable schema should preserve that distinction even if
they initially share one `vulkan` object.

### Hydra-adapter template

The host regression must install into an isolated staging prefix before
launching `testusdview`. Discovery or build-tree rendering is insufficient
evidence. The retained artifact needs each phase image plus a structured log
showing buffers written, depth/color coverage, validation count, scene/resource
revisions, aspect masks, draw count, dimensions, and completion value.

## Gaps and follow-ups

1. The repository has no enrolled self-hosted runner visible at repository
   scope. The workflow is manual and non-blocking until a runner with the
   `vulkan-1.4` GPU/driver label is enrolled.
2. The JSON schema currently covers Vulkan provenance only. Commit, build type,
   compiler, OS, resolution, and structural counters belong in the planned
   benchmark/result schema rather than being duplicated ad hoc.
3. Skip/error classification is still split across executable exit codes,
   CTest labels, and host assertions. OST should wait for the common
   machine-readable result schema before freezing this part of the template.
4. OpenUSD configuration and C++ runtime ABI compatibility are package
   provenance assumptions, not configure-time checks yet.

## Assessment

- `ost-template-render-validation`: the hosted/capability split, preflight,
  evidence retention, and deterministic labels are ready to extract.
- `ost-template-vulkan-backend`: runtime provenance fields are ready; converge
  them with the future benchmark schema before publishing a stable schema.
- `ost-template-hydra-adapter`: isolated install-tree execution and phase
  artifact retention are ready; OpenUSD ABI preflight remains renderer work.
