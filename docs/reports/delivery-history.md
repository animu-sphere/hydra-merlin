# Delivery history (pre-release roadmap detail)

This is the granular delivery log for work completed toward hdMerlin foundation
releases. It
is **historical evidence**, not a description of current behavior (see
[design](../design/)) or planned work (see [roadmap](../roadmap/)). Once a
version ships, its stable summary belongs in the
[changelog](../../CHANGELOG.md).

Legend: ✅ done

---

## v0.16.3 independent validation and closure — 2026-10-13

Release preparation starts from merge commit `3815552`, containing the backend/
shader refactor from `9499ed5`. The user also reports successful Vulkan operation.
No additional hardware or material support is inferred from that manual check.

[Capability run 38222886388](https://github.com/animu-sphere/hydra-merlin/actions/runs/38222886388)
on `9499ed5` completes successfully. RTX A5000 native Debug and Release each
pass 24/24 selected cases without skips, including conventional/bindless/GPU
Scene Forward, Gaussian, lifetime, validation and installed execution.
OpenUSD 26.08 Vulkan Lookdev discovery/bridge/RenderBuffer/viewport passes 5/5,
and the native/host/install selection passes 17/17. Its JUnit evidence checker
requires all 14 Forward/AOV/install cases to execute without skips. The
Release OpenStrata lifecycle also passes 46/46. Cache-save warnings are
infrastructure observations; execution and artifact upload succeed.

Downloaded evidence is under `build/v0163-vulkan-evidence`. Its Vulkan
Tier 0/Hgi comparisons contain 25 lighting, 393 motion and 433 navigation
image pairs with zero changed pixels, plus 21 exact Mesh/click pairs and
12 exact native/Tier 0 centers. The offline comparator uses these refreshed
Vulkan reports and the independently captured post-refactor Apple M3 reports:

| Comparison | Result | Saved report |
| --- | --- | --- |
| Native Debug | 20/20, maximum channel error 1/255 | `backend-native-debug-comparison.json` |
| Native Release | 20/20, maximum channel error 1/255 | `backend-native-release-comparison.json` |
| Tier 0 host | 24/24, zero error | `backend-host-tier0-comparison.json` |
| Hgi host | 24/24, zero error | `backend-host-hgi-comparison.json` |

All 17 hosted checks succeed on `3815552`, including Core/Metal/MaterialX,
Linux Vulkan/lavapipe Debug/Release and OpenUSD 26.05/26.08 compilation.
Hosted Metal runtime skips supply no additional GPU evidence; the local
Apple M3 Debug/Release and Hydra evidence below remains the device baseline.

The standard capability workflow does not enable MaterialX or select generated
Forward execution. A dedicated validation branch retains the same renderer
source and enables MaterialX generation plus handwritten/generated Vulkan
Forward in both configurations. Its
[run 38270938252](https://github.com/animu-sphere/hydra-merlin/actions/runs/38270938252)
passes 5/5 cases in each Debug/Release configuration without skips after the
user starts the self-hosted runner `DESKTOP-HJD3P8F`. Both configurations execute
MaterialX generation, prototype/Standard Surface Forward SPIR-V compilation,
generated Vulkan material rendering and handwritten material resources. A
separate JUnit gate rejects skipped or absent generated execution. Downloaded
evidence is under `build/v0163-generated-{debug,release}`. This completes the
remaining generated-path architecture gate; no v0.16.3 task remains active.
Independent report checks find exact generated/handwritten agreement in all
20 centers per configuration and generated Vulkan/Metal agreement within 1/255.

Release identity is prepared as `0.16.3`, dated 2026-10-13 JST. A fresh Core
Release build in `build/release-v0163-core` passes 23/23 without skips, including
installed consumption, backend-boundary, release-metadata and tooling checks.
The support matrix replaces the obsolete runtime-MSL description with the
build-time Slang/metallib contract; support scope remains unchanged. The
[release record](../releases/v0.16.3.md) summarizes this phase, and subsequent
exposure/tone-mapping, representative quality and material-parity work stays
in the roadmap.

## v0.16.3 local backend/shader architecture validation — 2026-10-12

Evidence was captured in the working tree based on `2ea1d30`, on Apple M3,
macOS 15.7.9, Apple Clang 17.0.0, Slang 2026.8 and Xcode 16.4's Metal compiler.
OpenUSD host checks use the local 26.08 shared runtime and Python 3.13.
This is pre-release evidence; `VERSION` remains 0.16.2.

| Configuration | Result | Scope |
| --- | --- | --- |
| Core Release | 23/23, no skips | CPU contracts and isolated installed consumption |
| Native Metal Debug | 35/35, no skips | Shader/ABI/cache, Forward, Gaussian compute/residency, GPU Scene, lifetime and installed runtime |
| Native Metal Release | 35/35, no skips | Same native gates |
| Metal/Hydra Release | 55/55 across the 44 remaining and 11 focused Forward cases, no skips | CPU/native/Hgi AOV/discovery/install gates plus usdview lighting, motion and Qt navigation |
| Vulkan Release, macOS compilation | All targets build/link; 31/31 selected GPU-free cases, no skips | Khronos Headers/Loader 1.4.321, shader/reflection/cache and installed Core/Vulkan consumers; GPU/runtime/benchmark cases explicitly excluded |

The native Metal color/white fixtures preserve all 44 pre-refactor PPM images
exactly in both Debug and Release. Comparing the saved host baseline with new
Tier 0/HgiMetal captures also finds exact pixels in all 1,768 images: 25 lighting,
401 motion and 458 navigation captures per presentation path. The independent
Tier 0/HgiMetal gates compare 25 lighting, 393 motion and 433 navigation image
pairs with zero changed pixels or mean-channel error. Existing replay, restored
AOV and warmed resource-counter gates retain their original thresholds.

New ABI checks validate native Slang Forward uploads, vertex attributes,
color/ID outputs, argument-buffer arrays, GPU Scene table strides and
presentation/color-copy bindings. Metal SDK installation verifies both embedded
metallibs, emitted sources/reflection and checksums. Source-boundary checks reject
the previous inline/runtime Metal shader library.

The Vulkan loader-only compilation environment has no Vulkan device. It cannot
establish new Vulkan image, validation-layer or host execution coverage, and
this evidence does not close v0.16.3. Independent Vulkan Debug/Release device and
Hydra runs, followed by refreshed cross-backend comparisons, remain in the
[current roadmap](../roadmap/current.md#active). Existing support claims and
rendering semantics are unchanged; generated Metal materials and broader
Forward quality remain separate work.

## v0.16.2 independent backend validation and closure — 2026-10-12

Release preparation starts from `fe8de56`. Refreshed Apple M3 captures above
validate the changed Metal profile; independent RTX A5000 captures use the
repository's Windows `vulkan-1.4` runner, MSVC 19.51 and driver 597.16.

The Debug and Release native jobs in
[capability run 38148962635](https://github.com/animu-sphere/hydra-merlin/actions/runs/38148962635)
each pass 24/24 cases without skips. That run's host jobs failed because the
SDK-only artifacts do not contain Usdviewq; the overall workflow is not a
successful host gate. Downloaded native evidence is retained separately in
`build/v0162-vulkan-evidence/native-{debug,release}`. Comparing each configuration
with independently captured Metal reports passes all 20 native centers with
at most one channel unit error.

Device host provisioning now uses the Vulkan Lookdev artifact for OpenUSD 26.08:

- OCI reference: `oci://ghcr.io/animu-sphere/openstrata-runtime-cy2026-lookdev@sha256:21cb5fe4c725c9918f6b18fe79b957cfec9d6032e3a40e2e8290ed2dcb70331f`.
- Artifact SHA-256: `03c5a8edcf9f362bebf9d84afd53f4e8e84b24edfa333ad375b0871a1feae463`.
- Official v26.08 testusdview launcher SHA-256: `41c7d691bd2e8c11a70559d27c0d287ddca44e9d7d5fc00a53c806ccfa4ec032`.
- Runner Python: 3.13.14 x64; CMake interpreter, library and includes are pinned
  together. SDK-only 26.05/26.08 hosted compilation remains separate.

The first Lookdev attempt, run `38150143929`, selected Python 3.14 for host
execution against Python 3.13 modules and failed imports. Run `38150687969`
failed setup-python because the runner's Windows PowerShell policy blocked
its installation script. The workflow now validates and selects the existing
Python 3.13.14 installation already used successfully by runtime validation.

[Run 38150958489](https://github.com/animu-sphere/hydra-merlin/actions/runs/38150958489)
passes all 14 required Forward/AOV/install cases with no skips. Its 12 native/
Tier 0 centers and 851 lighting/sweep/navigation image pairs match exactly.
Its generic Mesh smokes fail authored-normal handedness comparison, and their
comparison dependency does not run; the overall workflow is failed. These
failed attempts remain distinct from successful evidence.

That Mesh failure also reproduces locally on Metal. Temporary diagnostics show
`primvars/normals:indices`, while the cache only checked `primvars/normals` and
its child locators. Two corners therefore retained stale normal indices. The
adapter now also handles OpenUSD's name-suffixed index locator for normals,
colors and UVs. The existing exact handedness gate remains unchanged, and an
indices-only edit/restoration additionally requires image change/recovery,
exactly one primvar fetch, zero points/topology fetch and no triangulation.
Local Metal smoke plus distant-light selection passes 2/2 without skips.

[Final host run 38151987793](https://github.com/animu-sphere/hydra-merlin/actions/runs/38151987793)
on `d433da5` passes 17/17 selected cases without skips, including generic Mesh/
click smokes and their comparison. Discovery/bridge/RenderBuffer/viewport and
Lookdev runtime validation also pass. Its 12 native/Tier 0 centers, 851 Forward
images and 21 Mesh/click images match exactly. The JUnit evidence checker
confirms all 14 required Forward/AOV/install cases executed successfully.
Downloaded evidence is in `build/v0162-vulkan-evidence/openusd-26.08-final`.
The lighting comparison JSON omitted by the former upload glob is regenerated
from the retained images with `compare_usdview_smoke.py --forward-lighting-only`;
the workflow now explicitly archives that report too.

After the cache fix, the Metal host selection passes 16/16 again; an independent
bridge-disabled build passes native/lighting/Mesh selection 4/4. Its 12 centers,
851 Forward images and 21 Mesh/click images match the HgiMetal captures exactly.
Independent backend comparison is repeated with the final Windows artifacts:
20 native centers within 1/255 and all 24 host centers per mode with zero error.

Release preparation synchronizes VERSION/OpenStrata/changelog to `0.16.2` with
release date 2026-10-12 JST. A fresh local Core selection passes 23/23 and Metal
release metadata/tooling plus installed consumer/runtime passes 4/4 without
skips. All 16 hosted checks pass on source commit `d433da5`: Windows/Linux/macOS
Core, macOS Metal and Windows/Linux MaterialX Debug/Release, Linux lavapipe
Debug/Release and OpenUSD 26.05/26.08 GPU-free Hydra compilation. These source
and runtime results support the [v0.16.2 release record](../releases/v0.16.2.md).

Independent backend comparisons use the following commands, separately for
Tier 0 and Hgi presentation:

```sh
python3 -B tests/compare_forward_backends.py build/v0162-vulkan-evidence/native-release/build-native/tests/forward-images-handwritten build/metal-forward-lighting/tests/forward-images-metal build/v0162-vulkan-evidence/backend-tier0-comparison.json --vulkan-host build/v0162-vulkan-evidence/openusd-26.08-final/build/adapters/merlin-hydra2/forward-lighting-tier0-install/usdview-first-frame-forward-lighting.json --metal-host build/metal-hydra/adapters/merlin-hydra2/forward-lighting-tier0-install/usdview-first-frame-forward-lighting.json
python3 -B tests/compare_forward_backends.py build/v0162-vulkan-evidence/native-release/build-native/tests/forward-images-handwritten build/metal-forward-lighting/tests/forward-images-metal build/v0162-vulkan-evidence/backend-hgi-vulkan-comparison.json --vulkan-host build/v0162-vulkan-evidence/openusd-26.08-final/build/adapters/merlin-hydra2/forward-lighting-hgi-vulkan-install/usdview-first-frame-forward-lighting.json --metal-host build/metal-hydra/adapters/merlin-hydra2/forward-lighting-hgi-metal-install/usdview-first-frame-forward-lighting.json
```

Both comparisons pass all 20 native centers within 1/255 and all 24 host
centers with zero error. This closes the bounded shared-default-output gate,
not full-image cross-backend rasterization or custom HDR parity. Broader
Kitchen/interior quality, generated Metal materials, unrestricted navigation,
other hardware/SDKs and controlled timing remain in the backlog.

## Refreshed Metal default-output validation — 2026-10-11

Validated `fe8de56` with the Gaussian composition regression correction below
on Apple M3, macOS 15.7.9, AppleClang 17, Slang 2026.8 and OpenUSD 26.08.
These captures use `forward-sh-l2-reinhard/v1`, replacing the pre-alignment
Metal evidence for the current output profile.

```sh
cmake --build build/metal-forward-lighting --parallel 4
cmake --build build/metal-forward-lighting-debug --parallel 4
ctest --test-dir build/metal-forward-lighting --output-on-failure --no-tests=error --output-junit v0162-metal-tests.xml
ctest --test-dir build/metal-forward-lighting-debug --output-on-failure --no-tests=error --output-junit v0162-debug-tests.xml
cmake --build build/metal-hydra --parallel 4
ctest --test-dir build/metal-hydra --output-on-failure --no-tests=error --output-junit v0162-forward-host-tests.xml -R '^merlin-(hydra2-(forward-(lighting|motion|navigation).*|install-discovery|hgi-metal-aov)|metal-forward-lighting|backend-boundaries|install-consumer|metal-install-runtime)$'
cmake --build build/metal-hydra-tier0 --parallel 4
ctest --test-dir build/metal-hydra-tier0 --output-on-failure --no-tests=error --output-junit v0162-tier0-tests.xml -R '^merlin-(hydra2-forward-lighting.*|metal-forward-lighting)$'
```

Debug and Release each pass all 33 tests without skips. All 44 native analytic
centers have zero error; the 44 PPM images and two JSON reports match exactly
between configurations. Repeated/restored four-AOV gates, steady-state resource
counters and API/shader validation pass. White OFF is `[121, 122, 122]` in the
linear native AOV, retaining headroom; ON, rotated ON and zero energy are
`[64, 64, 65]`, `[58, 59, 60]` and `[14, 15, 17]`.

The initial Release run failed the mixed Mesh/Gaussian composition assertion,
which assumed an uncompressed opaque blue channel above 240. The corrected
regression independently captures the opaque Mesh and Gaussian-only alpha,
then checks their expected composite within one channel unit. It preserves
green occlusion, exact depth/picking and CPU/GPU differential checks rather
than lowering the old brightness threshold. The successful full reruns include
this correction; the initial failed log remains in `/tmp`.

The bridge-enabled host selection passes 16/16; the separately bridge-disabled
native/Tier 0 selection passes 3/3, both without skips. All 12 native/Tier 0
linear centers and all 851 Tier 0/HgiMetal images (25 lighting, 393 sweep,
433 Qt navigation) match exactly. White OFF host centers are `[121, 122, 122]`
linear and `[183, 184, 184]` sRGB. Immediate/settled replay and restored-image
checks, zero steady geometry uploads/pipeline creation, installed discovery,
consumer/runtime and native HgiMetal AOV checks pass. Host tests use API
validation; native/AOV tests additionally use shader validation.

JUnit, native images and host PNG/JSON/trace artifacts remain in the build
directories above; `/tmp/merlin-v0162-*` retains build/test logs. Independent
Vulkan/Metal comparison and release checks remain pending. Broader Kitchen,
generated Metal materials, other GPUs/SDKs, unrestricted navigation and timing
remain outside this bounded validation.

## Forward default output alignment — 2026-10-11

Moved the existing Radiance loader and Lambert-convolved l=2 SH projection into
the GPU-independent RenderBackend library. Core consumers can load/evaluate it;
all 27 embedded default RGB coefficients are checked against the packaged HDR
within 1e-6. Vulkan retains its configurable HDR path and native error boundary.
Metal's shared Forward shader now consumes those default coefficients and applies
Vulkan's Reinhard compression before linear RGBA8 output. Its internal host/MSL
material block grows from 48 to 192 bytes with size/offset assertions; the public
Metal options remain unchanged. Custom Vulkan environments are outside the
default-output comparison scope.

Native fixtures add +Y, -Y and +X normal-only environment samples. The Metal
analytic oracle and host fixtures now require white OFF headroom. Dark
environment-only channels may quantize to zero without allowing saturation.
Reports identify `forward-sh-l2-reinhard/v1`; the offline backend comparator
requires 20 native center pairs within 1/255 and optionally 24 same-presentation
host pairs within 2/255. It rejects stale profiles, missing/duplicate phases,
invalid RGB, saturation, uploads and pipeline creation. Four GPU-free boundary
tests cover acceptance/rejection; they are not backend execution evidence.

Base is `89389b6` plus the `codex/forward-output-parity` changes. Windows Release
validation uses MSVC 19.51.36256, NVIDIA RTX A5000/driver 597.16, Vulkan SDK
1.4.350.0, Slang 2026.8 and the existing assembled OpenUSD 26.08 SDK. The optional
Kitchen_set stage contains 1,788 meshes. Commands below run from a VS 2026
developer environment; the existing Vulkan and MaterialX build directories were
rebuilt before their tests.

```powershell
cmake --preset core -B build/release-v0.16.2-core -G "Visual Studio 18 2026" -A x64
cmake --build build/release-v0.16.2-core --config Release --parallel 4
ctest --test-dir build/release-v0.16.2-core -C Release --output-on-failure --no-tests=error --output-junit core-tests.xml

$env:VK_LAYER_PATH='C:/VulkanSDK/1.4.350.0/Bin'
ctest --test-dir build/forward-hgi-ninja -C Release -R '^merlin-(environment-lighting|shader-abi|vulkan-material-resources|hydra2-forward-(lighting|motion|navigation|scene).*|install-consumer|vulkan-install-runtime|hydra2-distant-light|forward-lighting)$' --output-on-failure --no-tests=error --output-junit forward-output-tests.xml
ctest --test-dir build/forward-hgi-ninja -C Release -R '^merlin-(vulkan-material-resources|hydra2-forward-lighting-native-comparison|install-consumer|vulkan-install-runtime|environment-lighting|shader-abi|forward-backend-comparison)$' --output-on-failure --no-tests=error --output-junit forward-output-final-tests.xml
ctest --test-dir build/mesh-materialx-validation -C Release -R '^merlin-vulkan-generated-material$' --output-on-failure --no-tests=error --output-junit forward-output-generated-tests.xml
python -B tests/forward_backend_comparison_test.py
```

Core passes 23/23, the final Vulkan/native/install selection passes 8/8, and
generated MaterialX compilation/runtime passes 4/4; no final tests skip. The
earlier 20-case Vulkan selection passed all 12 lighting/motion/navigation/scene
host cases but failed native material and install-consumer cases, preventing
their two dependent comparisons/runtimes. An old 1.3 validation-layer path
produced a warning; a valid dark environment channel hit the former positive
RGB bound; and install-consumer compilation lacked the VS developer environment.
The final rerun uses the 1.4 layer, the environment-only zero allowance and the
developer environment. Historical failed logs are retained separately from the
successful final JUnit reports.

All 12 native/Tier 0 linear center pairs have zero observed error. Handwritten
and generated Vulkan materials match at all 20 centers, including the new SH
directions. Tier 0/HgiVulkan comparisons match all 25 lighting, 393 sweep and
433 Qt navigation images exactly. The 649 Kitchen images pass unchanged
tolerances: maximum changed-pixel fraction `4.970961301066271e-5` and maximum
mean channel error `0.0015099294951988799`. Each host retains exact settled/replay
and restored-image gates, zero steady geometry uploads/pipeline creation and
clean diagnostics; these bounded capture tests do not measure interactive timing.

The final Metal fixture C++ source also compiles under MSVC, which verifies
portable source syntax only. Metal backend/MSL compilation, Debug/Release Apple
GPU native execution, refreshed Tier 0/HgiMetal lighting/sweep/navigation and
actual independent Vulkan/Metal report comparison remain pending. Earlier
Apple M3 evidence below predates this shader change and cannot satisfy those
gates. No remote Mac operation, version bump, release tag or parity support
claim is part of this change; v0.16.2 remains open.

## Metal Forward host lighting and navigation — 2026-10-11

Extended the existing Forward fixtures to Metal native/Tier 0 center comparison
and independent Tier 0/HgiMetal lighting, first-frame camera sweep and Qt
tumble/truck/zoom image comparison. Base is `8dac646` plus working-tree changes.
Local Release validation uses Apple M3, macOS 15.7.9, AppleClang 17,
Slang 2026.8 and the existing OpenUSD 26.08 package.

The installed plugin now supports
`MERLIN_HYDRA2_DISABLE_HGI_METAL_BRIDGE=1` at delegate creation. Tier 0
fixtures set this explicitly; HgiMetal cases require GPU copy and fail on
fallback. This corrects the previously misleading Metal `tier0` test name
when the bridge was built. Baseline counters prove four CPU AOV readbacks for
Tier 0 and one coverage-depth readback for HgiMetal. Every focused center
requires headroom except white OFF, which must exactly match the current
uncompressed `[255, 255, 255]` diagnostic response in linear and sRGB output.

Validation commands:

```sh
cmake -S . -B build/metal-hydra \
  -DMERLIN_SLANGC_EXECUTABLE=/tmp/merlin-forward-slang/bin/slangc
cmake --build build/metal-hydra --parallel 4
ctest --test-dir build/metal-hydra --output-on-failure --no-tests=error \
  --output-junit forward-host-tests.xml \
  -R '^merlin-(hydra2-(forward-(lighting|motion|navigation).*|install-discovery|hgi-metal-aov)|metal-forward-lighting|backend-boundaries|install-consumer|metal-install-runtime)$'
cmake -S . -B build/metal-hydra-tier0 \
  -DMERLIN_ENABLE_HGI_METAL_BRIDGE=OFF \
  -DMERLIN_SLANGC_EXECUTABLE=/tmp/merlin-forward-slang/bin/slangc
cmake --build build/metal-hydra-tier0 --parallel 4
ctest --test-dir build/metal-hydra-tier0 --output-on-failure --no-tests=error \
  --output-junit forward-host-tests.xml \
  -R '^merlin-(hydra2-forward-lighting.*|metal-forward-lighting)$'
```

The bridge-enabled final selection passes 16/16, including native/AOV,
installed discovery/consumer/runtime and backend boundaries. All 12 native/host
center pairs and all 851 host image pairs (25 lighting, 393 sweep and 433 Qt
navigation) have zero observed channel error. Each presentation path passes
256 immediate sweep and 192 immediate Qt captures with exact settled-reference
and restored-image checks, zero geometry fetch/upload and pipeline creation,
and zero validation messages. The CPU lighting/Hydra/consumer selection passes
6/6. The bridge-disabled build passes its native/Tier 0 lighting selection 3/3;
no final tests are skipped. Negative comparator checks accept a one-step difference and reject
two-step differences, wrong backends and missing phases.

Host runs enable Metal API validation. Enabling shader instrumentation for the
entire usdview process failed OpenUSD display pipeline creation and timed out;
that attempted selection was interrupted. The final host runner explicitly
disables shader instrumentation; native Metal and AOV gates separately retain
shader validation. GPU/window execution requires access outside the restricted
sandbox. Existing aggregate-initializer, Metal deprecation, Hydra switch/unused
and duplicate-library warnings remain; this is not warning-free build evidence.

JUnit remains in each build directory. Host PNG/JSON/log/Chrome-trace artifacts
live under `adapters/merlin-hydra2/forward-*-install`, with comparison reports
alongside them; native PPM/JSON artifacts remain under `tests/forward-images-metal`.
Metal Kitchen, generated Metal material ingestion, unrestricted navigation,
other GPUs/SDKs, cross-backend output alignment and timing remain unverified.

## Native Metal Forward lighting — 2026-10-11

Added the independent `merlin-metal-forward-lighting` GPU gate and archived
its `tests/forward-images-metal` artifacts in scheduled Metal capability CI.
Local validation uses Apple M3 (10 GPU cores), macOS 15.7.9, AppleClang 17,
Xcode 16.4 offline Metal tools and the SHA-256-pinned Slang 2026.8 arm64 archive
(`13949e0fc81f5c0bcde9a195190706740f1cc77af9c558f9f1b598e9bea3c606`).
Base is `75f7186` plus working-tree changes. Both builds disable Vulkan, Hydra,
MaterialX and the viewport; no OpenUSD runtime participates. No release or push
was performed.

Color/white patches each exercise 19 phases with an independent one-step
analytic center tolerance, three exact repeated AOV frames per phase, exact
camera return and fallback recovery, stable-handle selection/table compaction,
retained snapshots, extreme/degenerate axes and six rejected energy/color
inputs. Frame contexts are warmed before the zero-upload/allocation/pipeline
gates. CTest enables `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1`; native
device execution requires access outside the restricted sandbox.

Validation commands:

```sh
cmake --preset metal -B build/metal-forward-lighting -G Ninja \
  -DMERLIN_SLANGC_EXECUTABLE=/tmp/merlin-forward-slang/bin/slangc \
  -DMERLIN_BUILD_VIEWPORT=OFF
cmake --build build/metal-forward-lighting --parallel 4
ctest --test-dir build/metal-forward-lighting --output-on-failure \
  --no-tests=error --output-junit metal-tests.xml
cmake -S . -B build/metal-forward-lighting-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DMERLIN_ENABLE_VULKAN=OFF \
  -DMERLIN_ENABLE_METAL=ON -DMERLIN_BUILD_VIEWPORT=OFF \
  -DMERLIN_SLANGC_EXECUTABLE=/tmp/merlin-forward-slang/bin/slangc
cmake --build build/metal-forward-lighting-debug --parallel 4 --target \
  merlin-metal-forward-lighting-tests merlin-metal-backend-tests \
  merlin-forward-lighting-tests
ctest --test-dir build/metal-forward-lighting-debug --output-on-failure \
  -R '^merlin-(metal-forward-lighting|metal-backend|forward-lighting)$' \
  --no-tests=error --output-junit forward-lighting-tests.xml
```

Release passes 31/31, including Metal Gaussian/ABI/residency and installed
consumer/runtime regressions. Focused Debug passes 3/3; Core Release separately
passes 21/21. No tests are skipped. All 38 center samples have zero observed
analytic error; all 38 PPM images and both reports match exactly between Debug
and Release. Repeated/restored AOVs are exact, edit/static upload/allocation/
pipeline counters are zero and validation messages are zero. White OFF remains
255 under the existing Metal output profile; ON, rotated ON and zero energy
centers are 108, 98 and 38. This verifies the declared native diagnostic profile,
not host transfer functions, generated Metal materials, Kitchen/navigation,
cross-backend output parity or timing. Existing aggregate-initializer, Metal
`fastMathEnabled` deprecation and generated-MSL warnings remain in build logs;
this is not warning-free build evidence.

Local logs are `/tmp/merlin-forward-metal-{build,ctest}.log` and
`/tmp/merlin-forward-metal-debug-{configure,build,ctest}.log`; JUnit and
PPM/JSON artifacts remain in their build directories. Completed native Metal
selection validation is removed from current work; Vulkan/Metal lighting and
output alignment remains incomplete.

## Forward Kitchen starting viewpoints — 2026-10-11

Expanded the opt-in external scene gate from one frame-all view to three
independently derived free-camera poses: `full`, `quarter` (+45-degree yaw,
-15-degree pitch, 0.8 distance scale), and `opposite` (-45-degree yaw,
+15-degree pitch, 0.65 distance scale). Every pose runs Camera Light ON/OFF,
linear/sRGB and the eight-step Qt tumble/truck/zoom replay. Initial camera
transforms and recipes are recorded in `merlin-usdview-forward-scene/v2`; the
comparison rejects missing/reordered viewpoint captures or mismatched inputs.
The two additional views must visibly differ from each other and the full view.
Camera restoration, one-render exact settled-reference equality, zero geometry
fetch/upload, zero pipeline creation and clean validation retain their gates.

This fixture contains 288 immediate captures per presentation path and 649
Tier 0/HgiVulkan image comparisons. It uses the same locally supplied Kitchen
asset and assembled RTX A5000/OpenUSD 26.08 environment recorded below. Relative
camera offsets do not establish interior coverage, unrestricted navigation rate,
OpenUSD 26.05, Metal or broader lighting quality.

Initial `ctest --test-dir build/forward-hgi-ninja -C Release -R
'^merlin-hydra2-forward-scene' --output-on-failure` passes 3/3. Both paths pass
all 288 exact immediate/settled-reference captures with 1,788 draws, zero geometry
fetch/upload, zero pipeline creation and zero diagnostics/validation messages.
All 649 comparisons pass the existing bounds: maximum changed-pixel fraction
`0.00004970961301066271` (six pixels, about 0.005%), maximum mean-channel error
`0.0018268282781418546`. Tier 0 minimum coverage across the three views is
36,186 pixels. A stored-PNG saturation peak contains 17 pixels, all with alpha
178/255 in the inspected OFF/sRGB opposite-view truck capture. This is not proof
of displayed-color clipping. Large host traces and reports increased validation
overhead; the CMake Hgi gate now extracts each phase once before checking its
stages, retaining all existing transfer and availability assertions.

Final Release validation, including the recorded starting-camera assertion and
phase-local CMake checks, passes 19/19:

```powershell
ctest --test-dir build/forward-hgi-ninja -C Release `
  -R '^merlin-(hydra2-forward-(lighting|motion|navigation|scene).*|install-consumer|vulkan-install-runtime|forward-lighting|hydra2-distant-light|hydra-performance-report)$' `
  --output-on-failure
```

The selection includes the native material fixture, existing lighting/motion/
navigation comparisons, Core/Hydra lighting contracts, performance-report
contracts and installed Core/Vulkan consumers/runtime. Both scene paths again
pass all 288 exact captures. Final scene presentation comparison covers 649
images with maximum changed-pixel fraction `0.00004142467750888559` (five pixels,
about 0.0041%), maximum mean-channel error `0.0019738858832983987` and maximum
individual channel error 169. Sparse differences pass the aggregate bounds;
they do not establish exact presentation parity. The 17-pixel stored-PNG
saturation peak remains unchanged. Incremental Release configure/build, Python
syntax checks and `git diff --check` pass. The final local CTest log is preserved
at `build/forward-viewpoints-final-tests.log`.

## Forward external Kitchen navigation — 2026-10-11

Added opt-in `merlin-hydra2-forward-scene-{tier0,hgi-vulkan,comparison}` gates,
configured with `MERLIN_FORWARD_SCENE`. The stage opens directly; geometry,
materials, transforms, visibility and up axis remain authored. The fixture frames
the whole scene and isolates the host camera light by disabling authored lights.
Qt Alt+mouse tumble/truck/zoom routes reuse the focused navigation contract:
settled references, cloned-state replay, one-render immediate captures and exact
camera/image restoration. Camera Light ON/OFF and linear/sRGB toggles must change
the starting image. Every scene phase after setup forbids geometry fetch/upload
and pipeline creation. Scene identity, coverage, diagnostics, clipping fractions,
images and host traces accompany the independent presentation comparison.

Local validation uses Windows, RTX A5000 (NVIDIA 597.16, device Vulkan 1.4.329),
Release/Ninja, Vulkan SDK 1.4.350.0, Slang 2026.8.x and the assembled OpenUSD 26.08
host used for the focused lighting fixtures.
The locally supplied Pixar `Kitchen_set.usd` has 1,788 meshes, Z up and root-layer
SHA-256 `8f5cd40029d7bbb57b745a181b92e08515829236f9da4c9b2181851c7af5859b`.
That hash does not pin the referenced layers/textures; the asset is not bundled
or downloaded by the test infrastructure.

- The initial independent scene selection passes 3/3. Both presentation paths
  pass 96 immediate captures with exact settled-pose image equality. Captured
  draws remain 1,788; uploads, pipeline creation and validation messages are zero.
  Initial stored-PNG saturated-channel fractions are zero.
- The 217 Tier 0/HgiVulkan image comparisons pass the unchanged 0.25% changed-pixel
  and 0.25 mean-channel bounds. The initial run's maximum changed-pixel fraction
  is `0.000024854806505331357` (about 0.0025%), with maximum mean-channel error
  `0.0010584005103520268`; presentation is within tolerance, not exactly equal.
- HgiVulkan retains GPU copy without color Map or host upload. Baseline readback
  is 482804 bytes versus Tier 0's 1931216 bytes; timing samples are observations.
- Final `ctest --test-dir build/forward-hgi-ninja -C Release -R
  '^merlin-(hydra2-forward-(lighting|motion|navigation|scene).*|install-consumer|vulkan-install-runtime|forward-lighting|hydra2-distant-light)$'
  --output-on-failure` passes 18/18, including the native material fixture.
  The maximum scene changed-pixel fraction is `0.00004970961301066271` (six
  pixels, about 0.005%), maximum mean-channel error is `0.0011578197363733523`,
  and maximum individual channel error is 171. Sparse differences remain within
  the existing aggregate gates; they are not exact presentation parity.
  Both paths still pass all 96 exact first-frame/reference captures.
- The final `clipped_pixel_fraction` reaches `0.000024854806505331357` (three
  stored-PNG pixels), only in Camera Light OFF/sRGB captures. This field counts
  unassociated RGBA8888 RGB equal to 255. The inspected peak pixels are
  `(228,255,255,178)`, `(229,255,255,178)` and `(231,255,255,178)` in RGBA;
  their translucent alpha means this count alone does not establish displayed
  color clipping. Broad image quality remains independently gated.
- Incremental Release build, Python syntax and `git diff --check` pass.

This establishes bounded Kitchen overview navigation on one local host. It does
not establish arbitrary navigation rate, interior/multiple viewpoints, authored
scene-light quality, physically based lighting, OpenUSD 26.05, Metal or generated
material Hydra ingestion. The broader Forward lighting exit remains open.

## Forward Qt mouse navigation — 2026-10-11

Added independent `merlin-hydra2-forward-navigation-{tier0,hgi-vulkan,comparison}`
CTest gates. Qt Alt+left/middle/right mouse events exercise usdview's tumble,
truck and zoom handlers, including button release. Each eight-step route is
captured with settled frames, then replayed from a cloned free-camera state
without event-loop iteration or convergence waits. Both routes must produce
the same camera transforms; every immediate capture must record exactly one
renderer event carrying the camera change and equal its settled image exactly.
Camera restoration must also restore the starting image. The scene declares
a nonzero camera focus distance and a wider lens so pan/dolly are observable.
The Windows GPU workflow selects the gates and archives images, per-frame JSON,
structural counters and host traces. No renderer lighting/output policy changed.

Local evidence uses Windows, RTX A5000, Release/Ninja, Vulkan SDK 1.4.350.0,
Slang 2026.8.x and the same assembled OpenUSD 26.08 SDK/Usdviewq host documented
in the [2026-10-10 first-frame sweep](#forward-first-frame-camera-sweep--2026-10-10).
Both presentation paths complete all 192
immediate captures for color/white, linear/sRGB and Camera Light ON/OFF. Every
capture has exact settled-image equality and color headroom, with zero geometry
fetch/upload, new pipeline creation or Vulkan validation messages.

- `ctest --test-dir build/forward-hgi-ninja -C Release -R
  '^merlin-hydra2-forward-(lighting|motion|navigation)' --output-on-failure`:
  11/11 pass, including the existing lighting/motion gates and native material
  fixture dependency. All 433 navigation comparison images have changed-pixel
  fraction, mean-channel error and maximum-channel error zero.
- HgiVulkan retains GPU copy without color Map or host upload in every captured
  phase. Baseline readback is 482804 bytes versus Tier 0's 1931216 bytes;
  recorded timing samples are observations, not acceptance gates.
- The Release incremental build passes after initializing the MSVC development
  environment and selecting Vulkan SDK 1.4.350.0. The Core lighting, Hydra light
  energy, installed-consumer and installed-runtime selection passes 4/4 after
  that build. Python syntax, workflow actionlint (with the existing custom
  `vulkan-1.4` runner-label warning excluded) and `git diff --check` pass.

These are bounded synchronous Qt event/capture routes, not a frame-rate or
unrestricted user-navigation guarantee. Kitchen, OpenUSD 26.05 host execution,
Metal, generated-material Hydra ingestion and native swapchain output remain
unverified; the broader Forward lighting exit stays open.

## Forward first-frame camera sweep — 2026-10-10

Added independent `merlin-hydra2-forward-motion-{tier0,hgi-vulkan,comparison}`
CTest gates alongside the existing converged lighting fixture. A 17-pose,
0-to-30-degree yaw route advances and returns without idle/convergence frames.
For color/white, linear/sRGB and Camera Light ON/OFF, each framebuffer capture
must record exactly one render carrying the camera edit and exactly equal the
settled image of that pose. The tests retain separate images, per-frame JSON,
structural counters and Chrome traces; the Windows GPU capability workflow
selects and archives them. No renderer lighting or output policy was changed.

- Windows, RTX A5000, Release/Ninja, Vulkan SDK 1.4.350.0 and Slang 2026.8.x.
  This uses the existing OpenUSD 26.08 shared SDK in `build/hydra-ci-check` and
  `build/forward-hgi-ninja`, with a local wrapper importing the matching lookdev
  runtime's Usdviewq host. Hdx/HgiVulkan and renderer dependencies use the shared
  SDK. This assembled host is not an unmodified runtime-package claim.
- `ctest --test-dir build/forward-hgi-ninja -C Release -R
  '^merlin-hydra2-forward-(lighting|motion)' --output-on-failure`: 8/8 pass,
  including the native material fixture dependency and existing lighting gates.
  Both motion hosts pass all 256 immediate captures, including exact first-frame
  versus settled-pose equality and headroom. Geometry fetch/upload, new pipeline
  creation and Vulkan validation messages remain zero throughout the sweep.
- All 393 Tier 0/HgiVulkan images (baseline, 136 settled references, 256 moving
  frames) have changed-pixel fraction, mean-channel error and maximum-channel
  error zero. The bridge checks retain GPU copy without color Map or host upload.
  Baseline CPU readback is 482804 bytes versus Tier 0's 1931216 bytes. Timings are
  observations, not acceptance gates.
- Workflow actionlint and `git diff --check` pass. The custom `vulkan-1.4`
  runner-label warning is excluded from actionlint, as for the existing workflow.

The sweep updates an authored camera between synchronous screenshot operations;
it does not synthesize Qt mouse navigation or prescribe a frame rate. Kitchen,
unrestricted interactive navigation, OpenUSD 26.05 host execution, Metal,
generated-material Hydra ingestion and native swapchain output remain unverified.
The broader Forward lighting exit therefore stays open.

## Forward Camera Light fixtures — 2026-10-10

Moved from the Forward lighting design during the v0.16.x documentation
cleanup; the facts are unchanged.

Local Release validation on RTX A5000/OpenUSD 26.08 passes all five
native/Tier 0/HgiVulkan cases. The 25 presented images (baseline plus 24 fixture
phases) match exactly across Tier 0 and HgiVulkan; all 12 native/host center
comparisons and both color/white generated-material comparisons have zero
observed channel error. The native material run plus its three compilation
fixtures passes 5/5, and the Core lighting/Hydra energy/install-consumer/runtime
selection passes 4/4. These are local fixture results, not Kitchen, OpenUSD 26.05,
Metal, continuous-navigation, or timing guarantees. The separate lookdev SDK's
existing comprehensive smoke fails its authored-normal handedness image check
before reaching any lighting fixture; the independent gate remains usable.

## Forward directional identity and Hydra light energy — 2026-10-10

Implemented the shared single-directional selection/normalization and the
Hydra radiance-to-Lambert conversion described in the
[Forward lighting boundary](../design/forward-lighting.md). Stable light
handles survive dense table compaction. Both backends consume the common
directional result; Metal computes it once per render encoding. The Hydra
adapter converts angle, normalize, exposure and diffuse before committing a
light and rejects an invalid update atomically with named recovery.

- Windows x64, Visual Studio 2026/v143, Release, Vulkan SDK 1.4.350.0 and
  Slang 2026.8: Core passes 21/21 cases, including the shared CPU contract and
  installed consumer. OpenUSD 26.05 and 26.08 each pass all 38 selected
  GPU-free tests without skips, including the new actual Sprim transport
  test, shader/ABI, installed Core/Vulkan consumers and plugin discovery.
- The Vulkan device selection passes 7/7 related cases: shared input,
  shader/GPU Scene ABI, Gaussian raster, resource lifetime/update and material
  images. The light compaction image regression requires exact color/depth/ID
  output, zero geometry upload and no new pipeline. Vulkan material validation
  messages remain zero.
- The Hydra energy test independently integrates radiance over spherical caps
  at 0.53/10/60/180 degrees, checks normalize/exposure/diffuse and delta/zero
  cases, and captures rotation, parameter edits, clean Sync, atomic rejection,
  invalid initial input/retry and removal through the delegate. Both new CPU
  cases are now required by the hosted evidence checker.
- This is local evidence for the new code. The hosted run below predates this
  lighting work. Native Metal compilation/runtime and Camera Light ON/OFF
  Kitchen/host image differentials remain incomplete; the input conversion
  does not establish Vulkan/Metal output parity or the broader Forward exit.

## Hosted Hydra compilation gate — local validation, 2026-10-10

Added an independent Windows 2022 Release workflow for the pinned OpenUSD
26.05/26.08 Vulkan SDK artifacts. Both hosted configurations passed all 36
selected tests without failures or skips in
[run 37973871651](https://github.com/animu-sphere/hydra-merlin/actions/runs/37973871651)
for main commit `5b7754d` on 2026-10-10 (JST). Downloaded JUnit evidence and
successful selection/evidence-check steps confirm the CPU, shader/ABI,
installed consumers and plugin discovery gate. Host/device execution remains
separate. The local implementation history follows.

- Windows x64, Visual Studio 2026 generator with the v143 toolset (MSVC
  19.40.33821), Python 3.13.14, Vulkan SDK 1.4.350.0 and Slang 2026.8:
  both OpenUSD SDKs build Hydra, the native HgiVulkan bridge and shader artifacts.
  Each final GPU-free CTest selection passes 36/36 cases without skips and
  passes the evidence checker. Both selections also pass with Vulkan ICD
  discovery pointed at a nonexistent file. The Core-only configuration passes
  20/20 cases, including installed consumer linking and the Python include
  regression.
- The initial hosted configure failures exposed the 26.05 SDK's exact Python
  3.13.14 requirement and producer Python include directories retained in both
  SDK exports. The workflow now pins that patch; imported target includes are
  rebound after SDK version validation without changing artifact files. A
  configure regression covers an unavailable producer directory. Both SDKs
  build and pass the final selection using a separate consumer header/library
  root; the final hosted rerun also passes both configurations as recorded above.
- The next hosted run reached shader generation and exposed unset script-mode
  policies under CMake 3.31. The manifest script now initializes the project's
  CMake 3.24 policy baseline. CMake 3.31.6 reproduces the original failure and
  regenerates identical manifest contents except for the recorded CMake
  version with the fix.
- Both hosted SDK builds then passed, but CPU/package tests exposed the absent
  Vulkan loader DLL on GPU-free Windows runners. The hosted job now opts into
  a SHA-256-verified LunarG 1.4.350.0 runtime ZIP, adds only its x64 loader path
  and checks DLL loading/entry-point resolution without device creation.
  Capability jobs retain their system loader; no driver is installed.
- Installed plugin discovery uses the package fixture and checks the requested
  resource directory, with ambient plugin discovery paths cleared. Core and
  Vulkan installed consumers link separately from device/render execution.
- The gate disables post-link renderer reports, omits OpenStrata device/render
  validation and excludes the exact `gpu`, `runtime` and `host-smoke` labels.
  The Vulkan offscreen smoke was missing runtime labels; it now has them.
  Nine evidence-checker regressions cover missing/duplicate results, skips,
  failures, accidental device selection and enabled post-link execution.
- Pinned ost 0.23.8 successfully verifies/materializes the cached 26.08 SDK
  without runtime validation. VMA 3.3.0 provisioning works without an existing
  SDK header and rejects a corrupt header cache. Actionlint validates the new
  workflow and the capability workflow (with its existing custom runner label).
  Hardware execution, scheduled Metal capability checks and timing claims
  remain separate from the hosted compilation gate.

## v0.16.x Windows/Linux validation closure

Reactivated the previously deferred Linux Vulkan work and closed the active
follow-up scope on 2026-10-05 with Linux Vulkan coverage,
following the Vulkan 10M color-quality and HgiVulkan readback work below.
The Metal color-tail, macOS Mesh movement, HgiMetal readback and Apple M3 click
coverage have separate accepted records below. Broader GPU/performance and
HgiVulkan Qt click evidence remain incomplete in the next-cycle backlog.
This is a work-scope closure; it does not create a release tag or extend Metal
support claims.

- Ubuntu 26.04 / WSL2 x64, GCC 15.2, Slang 2026.8, LunarG SDK 1.4.350.1,
  Mesa 26.0.3 lavapipe (LLVM 21.1.8), loader 1.4.341 and device API 1.4.335:
  After integrating `b3b1c25`, Debug and Release each pass all 42 CTest cases
  with no skips, including the separate installed Vulkan runtime. Production
  SPIR-V passes `spirv-val --target-env vulkan1.4`. Gaussian CPU/GPU sorted
  and tiled parity, validation, resource lifetime/update, X11 viewport
  reference/resize and build/install consumers all execute on the CPU driver.
- A new hosted Linux Vulkan workflow defines the same Debug/Release checks
  separately from Core/MaterialX generation and hardware capability workflows.
  It records Mesa/driver evidence and rejects missing or skipped required
  runtime cases, a wrong driver/API or disabled validation. A small regression
  suite exercises those false-success cases. Hosted results remain distinct
  from these local captures.
- The first local run exposed a dedicated-memory assumption in the exhaustion
  test: lavapipe counts constructor buffers against its device-local budget.
  The test now leaves one byte beyond measured retained allocations and still
  requires the render-time image denial and unchanged retained memory. The
  adjusted lifetime test and evidence-checker regressions also pass on Windows
  RTX A5000/MSVC Release (2/2 CTest).
- Xvfb initially lacked its shared-library and keyboard-compiler dependencies
  locally. Installing the full distribution packages fixed the environment;
  the final full runs above include successful viewport execution. Existing GCC
  aggregate-initializer warnings are not claimed resolved. Linux native Wayland,
  Hydra hosts, real GPUs and hardware timing gates remain separate work.
- The first hosted build exposed GLFW's default OpenGL header inclusion in
  ImGui's input backend and native presentation. These GLFW includes now use
  `GLFW_INCLUDE_NONE`, so Vulkan/Metal compilation does not require unused
  OpenGL headers. Local final builds deliberately shadow `GL/gl.h` with a
  compile error to catch any residual inclusion. Linux SDK metadata reads
  the version above the architecture directory instead of reporting `x86_64`.

Raw local captures are in `build/lavapipe-debug/`, `build/lavapipe-release/`
and `build/lavapipe-windows-{build,test}.log`; the hosted workflow uploads
JUnit, validation/image artifacts, capabilities and X11 viewport evidence.
## v0.16.x click picking, overlay parity and CI separation

Completed the Apple M3 host and CI-separation slice on 2026-10-05, based on
`ff88c3c` plus the working-tree changes. Linux Vulkan work is deferred at the
user's request. Other Apple GPUs, controlled hardware timing and execution of
the added HgiVulkan click cases remain open; the Windows GPU runner was offline.

- Actual Qt left-button events traverse usdview's narrowed-frustum pick task
  and selection signal. They select `/World/OrientationProbe`, deselect on a
  background click, and select the moved probe after an odd-sized resize.
  Retina logical/physical coordinates are recorded with the selected path.
  Separate assertions prove the selection highlight and real OpenGL axes are
  visible and that deselection restores the unselected image.
- A bridge-disabled Metal build exposed upside-down Tier 0 output and a
  background click hitting an unrelated Mesh. Host projection reflection now
  follows the Hgi render driver independently of native-copy negotiation.
  Both bridge-disabled Tier 0 and HgiMetal pass the complete usdview sequence.
- The regression-only coverage depth readback had suppressed the native depth
  target. HdxAovInputTask returns early when native color is supplied, so it did
  not upload that CPU depth; subsequent OpenGL axis composition lost depth.
  Coverage readback now coexists with all four native AOV exports. Normal
  presentation remains demand-driven; diagnostic tests still read only their
  requested coverage depth. Click/selection phases permit required ID Map but
  continue to reject host upload of native color.
- Apple M3/macOS 15.7.9, AppleClang 17, Slang 2026.8 and OpenUSD 26.08:
  **all 21 Tier 0/HgiMetal Mesh images match exactly**, including prior edits,
  actual click hit/miss, moved/resized selection and visible/hidden axis
  captures. The existing changed-pixel (0.25%) and mean-error (0.25 channel
  units) limits are unchanged. Diagnostic baseline CPU readback remains
  5,158,080/1,289,520 bytes for Tier 0/GPU copy at 597x540; timings are local
  observations, not hardware gates.
- Hosted PR and SDK-release checks exclude only the exact `gpu`, `runtime` and
  `host-smoke` labels, preserving CPU GPU-Scene and GPU-driven contract tests,
  shader/ABI checks, MaterialX generation and package consumers. Installed
  Vulkan/Metal execution now has separate GPU cases with an install fixture.
  The weekly/manual Metal workflow retains Debug/Release runtime JUnit/logs and
  capability skips separately from build support; timings are never universal
  PR gates. Windows manual capability CI includes the installed Vulkan runtime.
- Core CTest passed **17/17**, Metal/Hydra CTest **34/34 without skips**, and
  native viewport/settings CTest **6/6 without skips**. The GPU-free filter is
  also exercised under the filesystem/process sandbox. Explicit SPIR-V
  validation and broader independent Hydra compile coverage remain carry-over
  work rather than newly claimed evidence.

Raw comparison/trace/click reports remain local in
`build/metal-hydra/usdview-metal-comparison.json`,
`build/metal-hydra/adapters/merlin-hydra2/usdview-install/` and the equivalent
`build/metal-hydra-tier0/` tree. Local JUnit results are retained in those build
trees and `build/metal-viewport-followup/` under the report policy.

## v0.16.x Metal quality, host AOVs and native viewport

Completed the locally testable Metal follow-up on 2026-10-05, based on
`589688b` plus the working-tree changes. Apple M3/macOS 15.7.9, OpenUSD 26.08,
Slang 2026.8 and AppleClang 16 were used. This closes the color-tail/larger
capture, Mesh movement and Metal demand-readback implementation items; broader
hardware, controlled timing, actual click picking/overlay coverage and CI
separation remain on the roadmap.

- Metal CPU/GPU sorted streams and tiles now accumulate in RGBA32F and convert
  once to the exported RGBA8 image, with pixel-center ellipse evaluation.
  A closed-form 512-splat low-opacity fixture independently checks the corrected
  reference. Offscreen 65K/1M tests pass the unchanged two-step color and exact
  depth/ID policies; the scale capture has zero color difference. Overflow,
  hidden/removal, mixed Mesh composition, leases and resize also pass.
- API-validated HgiMetal captures use forty warmed samples for each of six
  static/camera CPU/sorted/tiled phases at 597x540. The public 8,192-particle
  corpus has maximum sorted/tiled differences of 1/6 channel units, replacing
  the historical tiled 15-unit tail. The properly framed deterministic 1M scene
  matches exactly in all four final policy comparisons. Every measured GPU
  sample has zero prepared upload/allocation/fallback, zero image readback and
  zero bridge downloads; sorted/tiled control telemetry is 64/108 bytes.
- The generated scale scene lacked extent, so usdview originally displayed a
  small patch. The generator now authors conservative three-sigma bounds; the
  accepted 1M capture covers about 410x410 pixels. Old unframed captures remain
  local as limited evidence. Alternating camera phases end at their common
  reset camera; final images and per-sample counters provide distinct checks.
- Native viewport eight-frame tiled captures at 8,192 and 1M particles have
  maximum 1/255 CPU-reference differences at the physical 1194x1080 Retina
  extent. All eight frames select tile/float rendering, with no overflow or CPU
  fallback. A separate no-capture regression presents eight frames with zero
  image readback and exactly 864 counter bytes. Viewport reports now distinguish
  these bytes and actual path selection; Cocoa dialog parenting builds under ARC.
- HgiMetal copies depth and both ID targets alongside color. Exact current
  non-color Map tests cover motion, odd-sized resize, removal, cache reuse and
  Tier 0 recovery. ID Uint/Sint transfer stays on the GPU; Map owns aligned
  storage. Completion-handler lease release is serialized with backend state.
  Mesh projection reflection now flips front-face winding, and all usdview
  Mesh phases including Hdx selection pass. Regression logging still deliberately
  requests one depth image; normal benchmark frames request none.
- Full Metal/Hydra/native viewport CTest passes **39/39 without skips**, and
  Core CTest passes **17/17**. Offscreen and AOV tests run Metal API and shader
  validation. Large host captures run API validation with shader validation off;
  the shader-instrumented host stalled in Apple OpenGL-driver compilation before
  renderer submission. The failed attempt and stack sample remain local.
- Raster timestamps include final conversion; the retained float attachment
  costs 16 bytes/pixel per frame context. API/shader-instrumented offscreen
  warmed camera GPU medians are sorted/tiled 11.48/7.90 ms at 65K and
  64.77/51.60 ms at 1M; raster medians are 8.45/2.36 and 41.49/11.48 ms.
  These are local observations with unpinned power/clocks and some concurrent
  GPU work, not performance gates or additive stage estimates.

Raw reports/logs remain local in `build/metal-hydra/metal-host-8192-float-api/`,
`build/metal-viewport-followup/metal-host-1m-framed/`, native captures in the latter
build directory, and `docs/reports/2026-10-05-metal-v016-followup.md`.

## v0.16.x HgiVulkan demand-driven depth/ID readback

Completed the Vulkan slice on 2026-10-04, based on `7a85e96` plus the
working-tree change. Bound supported AOVs copy into retained Hgi targets with
completion-owned export leases. Host depth composition consumes its GPU
texture; non-color Map downloads the current target once. New GPU copies and
Tier 0 writes replace that CPU version. Mixed consumers and failed copies
retain eager Tier 0. HgiMetal remains color-only and is still on the roadmap.

- RTX A5000/OpenUSD 26.08/Windows Release/MSVC 19.51: the real HgiVulkan AOV
  test matches current depth/primId/instanceId exactly through motion, resize
  and removal, caches repeated Map, rejects mapped-buffer resize/mutation,
  and exercises Tier 0 writes. Synchronization validation exposed retained
  staging-write hazards; explicit reuse barriers fix both Hgi Map downloads
  and backend image readback. No device-wide waits were added.
- The 8,192-particle host CTest runs four samples per static/motion policy;
  a separate 1M host capture runs forty per policy. CPU sorted-stream, GPU
  sorted-stream and GPU tiled paths keep all four display images on the GPU:
  every measured sample has zero renderer image readback and zero bridge
  downloads. Final 1M policy images match exactly. Clocks/power remain unpinned;
  this is transfer/correctness evidence, not a universal performance gate.
- Mesh usdview still passes its existing image policy, and an added selection
  phase exercises demand-driven ID Map with validation. Its image comparison
  disables only usdview's subsequent OpenGL axis overlay; the original phases
  and thresholds are unchanged. Regression coverage deliberately reads back
  one depth AOV (1,289,520 bytes at 597x540), versus Tier 0's four AOVs
  (5,158,080 bytes). Normal host benchmarks disable that diagnostic coverage.
- A clean rebuild and all 54 CTest cases pass without skips, including
  source/install consumers, shared shader/ABI checks and Vulkan/Hydra fixtures.
  The first incremental run exposed an obsolete render-settings test object
  after the adapter telemetry layout changed; the clean rebuild resolved it.
  Metal runtime execution and broader actual host picking remain unverified.

Raw reports/logs remain local in `build/hydra-aov-*` and the dated
`docs/reports/2026-10-04-hydra-demand-readback.md` report.

## v0.16.x Vulkan Gaussian float composite and 10M quality closure

Completed the Vulkan color-discrepancy follow-up on 2026-10-04, based on
`b0db2b5` plus the working-tree change. CPU/GPU sorted streams and compute tiles
now use an RGBA32F working attachment and one final conversion to the public
RGBA8 AOV. Fragment ellipse evaluation uses the actual pixel center. Per-splat
UNorm quantization and interpolated quad offsets both contributed to the old
discrepancy; a closed-form 513-particle low-opacity RGBA fixture anchors the
corrected reference independently of GPU path comparisons.

- RTX A5000, driver 597.16, Vulkan SDK 1.4.350.0, Windows Release/MSVC 19.51:
  1M/5M/10M captures at 597x540 use forty warmed samples per static/motion path.
  All twelve final-frame comparisons have exact color, depth, primId and
  instanceId parity. The six-step tiled color bound is unchanged. Separate
  four-sample captures at every scale pass with validation enabled and no
  renderer validation messages.
- Gaussian raster timestamps include the final GPU conversion. At 10M,
  static GPU sorted-stream/tiled raster medians are 37.67/1.59 ms, total GPU
  medians 60.31/57.23 ms, and total frame medians 62.12/59.14 ms. The extra
  float target costs 16 bytes/pixel per frame context before allocation
  alignment. Clocks/power are unpinned; these are observations, not hardware
  timing gates, and stage medians are not additive frame estimates.
- Final CTest passes 53/53 with no skips, including shader/ABI, four-AOV
  Gaussian regression and overflow, completion/lease/resize, GPU-only RGBA8
  export, Mesh/Gaussian usdview, HgiVulkan synchronization/copy and installed
  SDK consumers. Tier 0 and HgiVulkan color-copy PNG bytes match. A separate
  float-composite Vulkan PNG records the intentional reference correction;
  the old shared PNG and host image tolerances are retained.
- Raw accepted, failed and rejected diagnostic reports remain local in
  `build/v016x-tile-quality/`, with the dated local
  `docs/reports/2026-10-04-vulkan-gaussian-tile-quality.md` report. The historical
  v0.16.0 10M 8/10-step failure remains untouched. Metal Gaussian execution and
  its host color-tail investigation remain separate incomplete work.

## v0.16.0 Metal Gaussian tile raster and Apple M3 scale check

Connected the tiled request to the native Metal renderer on 2026-09-30. The
same retaining command now performs sorted-record gather, tile pair count and
prefix scan, pair emit, stable tile-key radix sort, ranges and selection. A
successful selection skips the indirect sorted draw and composites into the
opaque Mesh color/ID targets; pair overflow or a clamped record retains the
complete sorted draw without a CPU-visible scheduling dependency. Tiled Prefer
uses GPU sorted-stream fallback if tile allocation is unsupported; Tiled Require
reports Unsupported for that condition. An explicit pair capacity supports
deterministic overflow checks.

- Release/AppleClang 17, Xcode 16.4, Slang 2026.8, macOS 15.7.9 and Apple M3
  were used at 512x512, 65,536 and 1,048,576 deterministic degree-0 particles.
  The source checkout was based on `3d5d015` with these changes uncommitted.
  `merlin-metal-gaussian-tests --tile-scale` captured cold plus five camera
  warmups and five measured camera frames per path with four AOVs requested.
  Every tiled frame selected compute raster. Color differed by at most 2/255
  per RGBA8 channel; depth and both ID
  AOVs matched GPU sorted-stream exactly. The image/overflow renderer test ran
  with Metal validation enabled. A separate 300-particle/256-pair test forced
  overflow and matched every sorted-stream AOV byte exactly.
  Metal API Validation renderer tests also cover tiled opaque Mesh depth,
  visibility/removal, empty and all-rejected frames, plus cold/warm Prefer
  fallback for an unsupported explicit capacity.
- Camera-frame medians (five measured samples, ms) at 65K: sorted GPU execution
  3.93, raster 2.15; tiled GPU execution 4.29, tile work 0.86, compute raster
  0.92. At 1M: sorted GPU execution 28.77, raster 12.56; tiled GPU execution
  35.10, tile work 11.34, compute raster 7.18. The tile raster stage is cheaper
  at both sizes, but binning and sorting outweigh the saving on these scenes.
  These are local samples with unpinned clocks/power, not a universal
  performance claim. Wall medians with
  four-AOV readback were 4.86/5.22 ms at 65K and 30.63/36.64 ms at 1M
  (sorted/tiled). Raw CSV remains local under the [report policy](README.md).
- A subsequent same-day HgiMetal GPU-copy/usdview run on the public
  8,192-particle corpus selected tiled raster
  in every measured frame, with 14,169–14,231 requested pairs below the 65,536
  capacity, zero overflow/fallback and zero warmed uploads/allocations. Four
  samples per static and alternating-camera phase passed the host image policy:
  tiled versus CPU had maximum RGBA8 channel difference 15, with only 0.0183%
  of pixels above the six-unit per-channel threshold and mean channel error
  0.0172. This measured tail exceeds the offscreen degree-0 two-unit bound;
  it remains a quality/tuning target. Camera-phase median GPU execution was
  3.45 ms tiled versus 2.79 ms sorted; tile work took 0.42 ms and compute raster
  1.43 ms. A separate two-sample HgiMetal tile run passed with Metal API and
  GPU order/pair validation enabled. The four timing samples are directional
  evidence only. HgiMetal tiled 1M,
  native viewport captures, other Apple GPU models and scene tuning remain
  open. Earlier HgiMetal comparisons below record the former sorted-stream
  fallback and remain valid historical evidence.

## v0.16.0 Metal Gaussian host validation and stage timestamps

Completed controlled HgiMetal sorted-stream comparisons and device stage
sampling on 2026-09-30. Phase 3 tile raster and broader hardware evidence
remain open; a tiled request in `Prefer` is explicitly counted as a sorted-stream
fallback.

- Release/AppleClang 17, Xcode 16.4, Slang 2026.8, OpenUSD 26.08,
  macOS 15.7.9 and Apple M3 were used with 597x540 HgiMetal GPU-copy output.
  The public 8,192-particle degree-3 corpus has 40 warmed samples per static
  and alternating-camera phase; the generated degree-0 1M scene has 20.
  Each capture records the scene, runtime config and installed renderer hashes,
  policy, image comparison and per-sample counters. Validation was off for
  timing; a separate four-sample HgiMetal host comparison and the Metal GPU
  image, residency and compute tests pass with validation enabled.
- Four GPU-versus-CPU host screenshots per corpus pass: maximum RGBA8 channel
  difference is 1 on 8,192 particles and 0 on 1M, with zero pixels above the
  sorted-stream two-step threshold. Every GPU camera sample has zero CPU
  Gaussian preparation, zero attribute/prepared upload and zero allocation.
  HgiMetal copies color on the GPU and reads back three other AOVs in one map:
  3,868,560 image bytes at this extent, plus 64 bytes of post-completion
  counters on the GPU path. Direct sharing is still unavailable.
- Apple stage-boundary counter samples now report preparation including scratch
  clear/key generation, radix sort including gather, and an isolated Gaussian
  render pass. On the 1M moving phase their medians are 2.30, 12.33 and
  99.86 ms, with 114.81 ms command-buffer GPU execution. The CPU reference
  moving phase spends 66.68 ms in CPU preparation, uploads a 52 MB prepared
  stream and has a 100.96 ms GPU execution median. On this corpus the GPU
  path removes CPU traversal/upload but does not improve device raster cost;
  static CPU cache remains cheaper. The render-pass sample includes its
  load/store overhead, and stage medians are not additive frame estimates.
- The Metal/Hydra CTest run passed 31/32 cases. The remaining Mesh usdview
  smoke fails an image movement assertion in the same way on an isolated
  build of the unmodified `2cabe45` commit; it does not exercise Gaussian
  rendering. This pre-existing host test gap is separate from the passing
  Gaussian host captures.
- The Phase 3 Metal tile shaders compile into the packaged metallib. On Apple
  M3, every tile pipeline is created and a two-tile fixture passes GPU
  count/emit/ranges/verification plus compute raster color/ID checks under
  Metal API validation. Its known prefix and ordered pairs are supplied by
  the test; the renderer's frame-wide scan/sort, GPU overflow choice and
  native tiled mode remain to be connected and compared at scale.

Raw captures remain local under the [report policy](README.md). Reproduce the
same-runtime HgiMetal captures with the [Metal host commands](../guides/benchmarking.md#metal-hgimetal-host-comparison).

## v0.16.0 Vulkan Gaussian and host comparisons

Completed the Vulkan Gaussian/host measurement work on 2026-09-30. The harness
records image failures as failures; the 10M tiled color discrepancy and the
overall v0.16.0 exit criteria remain open.

- Release/MSVC 19.51, RTX A5000 driver 597.16, 597x540 and 40 warmed samples per
  phase cover deterministic degree-0 1M/5M/10M particles. CPU, GPU sorted-stream
  and GPU tiled replay the same camera sequence. GPU preparation, sort, tile
  binning and raster use device timestamps; every measured GPU frame skips CPU
  preparation with zero source/prepared uploads, allocation and stage fallback.
  Compute descriptors still refresh and are reported. Validation is disabled
  for timing; separate 1M/5M validation captures pass.
- Sorted-stream matches all four CPU AOVs exactly at all three tiers. Tiled
  preserves depth/primId exactly and stays within the established 1% depth-tied
  particle-ID allowance. Maximum RGBA8 error is 3 at 1M and 5 at 5M; 10M reaches
  8 static/10 moving against the six-step bound and returns failure with its
  complete report. Its timings are diagnostic, not acceptance evidence.
- Moving CPU preparation medians are 113.93/598.50/1311.91 ms at 1M/5M/10M;
  GPU preparation is 0.267/1.275/2.671 ms on sorted-stream. Sorted-stream total
  frame medians are 5.86/26.17/45.82 ms, including four-AOV CPU readback.
  Static cached CPU can be cheaper than tiled recomputation at 1M. Clocks and
  power are unpinned, so these serialized local observations are not timing gates.
- Paired usdview captures use the same OpenUSD Vulkan runtime for HgiGL Tier 0
  and HgiVulkan GPU copy, the 8,192-particle degree-3 corpus and deterministic
  1M scene, six static/motion policy phases and 40 samples each. All twelve
  cross-host phase images match exactly. Every GPU-copy frame reads/maps three
  depth/ID AOVs instead of four: 3,868,560 versus 5,158,080 bytes, saving
  1,289,520 bytes (25%). Color copy has no coarse wait; at most one lease is
  pending. Direct share remains rejected as `public-texture-import-unavailable`.
  Host camera framing differs from the headless experiment. Host copy/present
  scopes are CPU trace durations, not GPU copy or end-to-end display timings.
- The public artifact is
  `oci://ghcr.io/animu-sphere/openstrata-runtime-cy2026-lookdev:26.08-vulkan-windows-x86_64`,
  OCI digest `sha256:21cb5fe4c725c9918f6b18fe79b957cfec9d6032e3a40e2e8290ed2dcb70331f`,
  archive digest `sha256:03c5a8edcf9f362bebf9d84afd53f4e8e84b24edfa333ad375b0871a1feae463`.
  OST verifies its SBOM/provenance and installs it into an isolated local SDK;
  hdMerlin is rebuilt against that SDK.
- Hgi synchronization validation exposed overlapping transfer writes to the
  tiled verification record count. An explicit transfer-to-transfer buffer
  dependency fixes it. The new install-tree host benchmark checks all six
  policies with synchronization validation; related Vulkan/Hgi/image/report/
  install-consumer tests pass. Fifteen focused tests also pass on the prior
  GL lookdev SDK.

Raw reports, traces, images and the dated report remain local per the
[report policy](README.md). Reproduce them with the
[Gaussian comparison commands](../guides/benchmarking.md#gaussian-comparisons).
Metal host comparisons and stage timestamps are recorded above; tile raster
remains open. These Vulkan results do not extend their support claim to Metal.

## v0.16.0 Vulkan Mesh scaling validation

Validated the supported Mesh submission and generated-material fallback boundary
on 2026-09-28. This completes the Mesh validation work item; remaining Gaussian
acceptance issues and the overall v0.16.0 exit criteria remain open.

- Eight opt-in configurations cover shared/diverse/textured Mesh, 2/4/8/16
  production vertex-arena blocks, and conventional generated-material execution
  at 1k/10k/100k draws. Diverse fixtures retain 16 triangle/quad resources and
  eight materials; textured fixtures add four images and two samplers.
- Static and camera-motion GPU phases report zero CPU preparation draw visits,
  geometry/texture/GPU Scene uploads, allocations and descriptor writes after
  warming. Per-frame material uniforms still refresh outside the upload counter.
- The correctness gate records 78 exact color/depth/primitive-ID/instance-ID
  comparison sets with rasterized foreground. Textured/arena fixtures also
  compare against an independent Forward renderer without GPU Scene tables.
- Generated artifacts execute conventionally with zero material fallback.
  Required GPU submission rejects; preferred mode records one submission
  fallback. Parameter edits and missing-module/restoration exercise invalidation
  and image recovery. Unique-variant preflight and completed-frame descriptor
  reuse remove warmed preparation traversal and descriptor churn. Conventional
  draw recording remains linear and is excluded from the preparation counter;
  generated bindless/GPU-driven execution remains unsupported.
- All 41 selected source/runtime/install-consumer CTest cases and four actual
  MaterialX compiler/artifact/runtime cases pass. The eight GPU configurations
  pass with validation enabled and zero renderer-owned diagnostic messages.

Local timing used Release/MSVC 19.51, Vulkan SDK 1.4.350.0, an NVIDIA RTX A5000
(driver 597.16), a fixed 256x256 four-AOV target and 30 warmed samples per phase.
Textured static preparation medians were 4.60/5.60/5.45 us at 1k/10k/100k draws;
moving-camera preparation medians were 5.75/5.00/6.20 us. Recording grows with
arena/pipeline batch count (up to 128 batches in the 16-block fixture).

These are serialized, same-device headless observations with clocks and power
unpinned, not universal timing gates, controlled host comparisons, or renderer
before/after measurements. The vertex sweep pads unreferenced vertices while
keeping index geometry in one block; arbitrary index-block packing and growing
material/module counts are outside this evidence. Raw captures remain local per
the [report policy](README.md). Reproduce the correctness gate and timing captures
with the fixtures and commands in the [benchmark guide](../guides/benchmarking.md).

## v0.14.1 Gaussian correctness MVP ✅

- ✅ Added standard Hydra `particleField` ingestion, float/half normalization,
  covariance evaluation, degree-0–3 spherical-harmonic payloads, host-neutral
  revisions, normalized particle ranges, and structured diagnostics without a
  custom USD schema or direct PLY/SPLAT parsing.
- ✅ Added deterministic CPU camera-space preparation, projection, culling,
  stable back-to-front sorting, procedural Vulkan ellipse rasterization,
  authored opacity/SH composition, static cache reuse, and stage telemetry.
- ✅ Added frame-owned packed-stream shadows so localized edits upload only
  changed prepared-instance ranges while reorder/visibility/size changes remain
  correct and completion-safe.
- ✅ Defined Gaussian picking as resource `primId` plus zero-based particle
  `instanceId`; a dedicated ID subpass retains the nearest contributing
  Gaussian without requiring `independentBlend` and preserves uncovered Mesh
  identity.
- ✅ Added the repository-owned CC0 8,192-particle degree-3 USDC corpus and a
  visible 597×540 reference image, with Tier 0 and HgiVulkan usdview comparison
  gates and zero renderer-owned Vulkan validation diagnostics.
- ✅ Captured local native Vulkan viewport evidence for 300 hidden VSync-off
  frames at 597×540: 1,536,743 ns average CPU frame time, 300 presented frames,
  386,856,000 presentation-copy bytes, and zero validation messages.

## v0.14.0 HgiMetal GPU-copy host presentation ✅

- ✅ Added HgiMetal render-driver discovery, Hgi-owned color targets, and
  same-`MTLDevice` GPU copy from leased Merlin color AOVs on validated OpenUSD
  26.05/26.08 packages, while preserving Tier 0 CPU RenderBuffers as the
  universal fallback.
- ✅ Added explicit capability, fallback, target-lifetime, copy, upload,
  completion, and direct-share evaluation telemetry, with completion-safe
  target retirement across resize and driver changes.
- ✅ Kept direct sharing affirmatively gated on device, storage, usage, pixel
  format, queue, completion, retirement, public-import, and implementation
  requirements; validated packages reject unavailable public texture import and
  select GPU copy.
- ✅ Hardened Forward lighting for stable signed normals and camera-light
  contribution without changing the Vulkan projection orientation.

## v0.13.1 HgiVulkan direct-path hardening ✅

- ✅ Added one affirmative direct-share gate covering physical/logical device
  identity, queue ownership, API/extensions, format/usage, sample count,
  tiling, memory constraints, public import and host consumption, completion
  retention, resize retirement, and implementation availability.
- ✅ Added stable per-gate rejection names and evaluation telemetry separately
  from the selected transfer fallback, so rejecting Tier 2 does not misreport a
  supported Tier 1 GPU-copy frame as an operational failure.
- ✅ Added sampled usage plus explicit usage, tiling, device-local memory, and
  exclusive-sharing metadata to exported color images, and made GPU copy
  validate the expanded source contract.
- ✅ Confirmed that OpenUSD 26.05/26.08 public Hgi creates Hgi-owned textures
  and aliases Hgi-owned sources but cannot import a Merlin-owned `VkImage`;
  both packages therefore report `public-texture-import-unavailable` and retain
  GPU copy instead of depending on private Hgi ownership internals.
- ✅ Passed self-hosted GPU capability
  [run 30655056809](https://github.com/animu-sphere/hydra-merlin/actions/runs/30655056809)
  at commit `8a2b4a4`: OpenUSD 26.05/26.08 HgiVulkan, Vulkan 1.4 Debug, and
  Vulkan 1.4 Release all completed successfully, including both 13-phase
  Tier 0/HgiVulkan usdview regressions.

## v0.13.0 HgiVulkan GPU-copy bridge ✅

- ✅ Added public Hydra-driver discovery, Hgi-owned color targets, and selected
  color GPU copy on the validated OpenUSD 26.05 and 26.08 packages while
  retaining Tier 0 CPU RenderBuffers as the universal fallback.
- ✅ Added a borrowed Vulkan 1.3 device/graphics-queue path for HgiVulkan hosts,
  with explicit image layout/ownership metadata and no device- or queue-idle
  wait; Merlin-owned Vulkan remains at the Vulkan 1.4 baseline.
- ✅ Exported selected renderer AOV images through move-only leases that retain
  frame targets through Resolve and return from Hgi command-buffer completion;
  depth and ID AOVs remain on CPU readback.
- ✅ Added structured selection, fallback, copy, byte, completion, and
  coarse-wait telemetry plus fallback coverage for missing, disabled,
  non-Vulkan, swapped, and operationally rejected host paths.
- ✅ Exercised Tier 0 and HgiVulkan through the same 13 scene/camera/material/
  diagnostic/lifetime/resize phases with bounded image differences and
  versioned readback/upload/copy performance evidence.

## v0.12.0 native Metal presentation ✅

- ✅ Added a private Cocoa/GLFW adapter that creates the `CAMetalLayer` while
  `Merlin::Metal` configures its device, pixel format, color space, pacing, and
  drawable pool and retains all command encoding and completion ownership.
- ✅ Added a renderer-owned fullscreen GPU presentation pass from the reference
  RGBA target to BGRA sRGB drawables, including linear sRGB-to-Display P3
  conversion and an explicit rejected boundary for future HDR output.
- ✅ Integrated the official Dear ImGui Metal renderer into the shared
  viewport UI without moving native handles or widget ownership into Core.
- ✅ Added resize-safe drawable extents, frames-in-flight drawable retention,
  vsync and drawable-count policy, presentation timing/copy telemetry, and
  zero-readback normal frames.
- ✅ Made `merlin-viewport` build and package with Metal alone while preserving
  explicit Vulkan selection and Apple automatic Metal preference in combined
  configurations.
- ✅ Exercised hidden compositor-backed Apple GPU presentation, resize,
  validation, exact presented/offscreen reference parity, Metal presentation
  target validation, Display P3 configuration, and explicit HDR rejection in
  Debug and Release test suites.

## v0.11.0 native Metal backend ✅

- ✅ Added the optional `Merlin::Metal` backend with native device/queue,
  renderer-owned runtime MSL compilation, Forward pipelines, depth, offscreen
  targets, and explicit submit/completion/resolve behavior.
- ✅ Consumed the same host-neutral Mesh, instance, camera, texture, sampler,
  basic material, opacity-mask, and render-product records as Vulkan, with
  color, depth, `primId`, and `instanceId` CPU readback.
- ✅ Added heap-backed scene residency, finite generation-checked texture and
  sampler slots, completion-protected reuse, frames-in-flight target/readback
  reuse, argument-buffer tier negotiation, and conventional Forward fallback.
- ✅ Made per-frame argument buffers dirty-only and retained zero target
  allocation and zero table update for a steady snapshot after frame contexts
  are seeded.
- ✅ Added actionable heap/table exhaustion classes and backend-neutral plus
  Metal-specific capacity, residency, allocation, retirement, and argument
  update telemetry.
- ✅ Exercised textured output, depth/picking identity, alpha-mask discard,
  replacement under stable slots, install-tree consumption, and runtime command
  completion on Apple Silicon; hosted macOS Debug/Release jobs retain
  compile/package evidence.

## v0.10.0 MaterialX shader-generation boundary ✅

- ✅ Generated the accepted constants, image/UV0/world-normal,
  add/multiply/mix, and minimum Standard Surface slice as deterministic,
  pass-neutral Slang material functions.
- ✅ Kept topology, parameter state, resource assignments, and target artifacts
  separately identified and verified the common ABI against SPIR-V and Metal
  reflection.
- ✅ Executed parameter and texture/sampler modules in renderer-owned Vulkan
  Forward with per-module descriptor/pipeline layouts, pipeline reuse across
  value and texture-content edits, and structured artifact/resource fallback.
- ✅ Added GPU image evidence for textured Standard Surface execution and
  missing-resource recovery without changing depth, picking, lighting, or AOV
  ownership.
- ✅ Serialized generated draw/fallback evidence through benchmark,
  renderer-report, native viewport, and Hydra regression reports.
- ✅ Retained and installed the accepted generated Slang, SPIR-V, Metal-target,
  and reflection artifacts under `shaders/v2/materialx`.

## Renderer foundation ✅

- ✅ Added the handle-based, host-neutral `RenderWorld` scene model.
- ✅ Added deterministic draw extraction without OpenUSD, Hydra, Vulkan, Qt, or
  DCC SDK types in the Core public API.
- ✅ Added a persistent Vulkan offscreen renderer with color/depth CPU readback,
  multiple frame contexts, revision-based scene upload, and one completion
  value for both render products.
- ✅ Added the opt-in Hydra 2 adapter with mesh topology, transform, visibility,
  camera synchronization, adapter-owned USD-path mapping, CPU RenderBuffers,
  and a Vulkan-backed render pass.
- ✅ Added deterministic Core, RenderBuffer, Vulkan offscreen, validation, and
  install-tree usdview stable-update tests.

## Vulkan 1.4 baseline ✅

- ✅ Made Vulkan 1.4 the minimum source-build, installed-package, loader, and
  physical-device API contract.
- ✅ Required a Vulkan 1.4-capable graphics queue and `glslc` at build time.
- ✅ Kept renderer-owned validation and performance warnings as failures while
  separating unrelated general loader or host diagnostics.
- ✅ Added versioned JSON evidence for SDK/header, loader, selected device,
  driver, API, timeline semaphore, and validation state.
- ✅ Retained the JSON evidence and render logs/images as capability-workflow
  artifacts.

## Reproducible CI and package baseline ✅

- ✅ Added Windows and Linux Core-only Debug and Release CI.
- ✅ Exercised source tests and the isolated install-tree consumer in hosted CI.
- ✅ Split hosted Core, Vulkan/headless, and OpenUSD/Hydra work by required
  capability, with explicit skip semantics for optional capabilities.
- ✅ Added a manually dispatched Windows capability workflow with separate
  headless and Hydra jobs and a `vulkan-1.4` runner-label contract.
- ✅ Pinned GitHub Actions, the LunarG Vulkan SDK download checksum, the
  OpenStrata CLI version, and the Animusphere OpenUSD runtime artifact digest.
- ✅ Kept assertion-based tests active in Release configurations and cancelled
  superseded Core CI runs per ref.
- ✅ Installed versioned CMake package files and exported `Merlin::RenderWorld`,
  `Merlin::RenderExtraction`, and optional `Merlin::Vulkan` targets.
- ✅ Defined Headless, benchmark, and Hydra as runtime-only install products so
  OpenUSD stays out of the exported Core/Vulkan dependency graph.
- ✅ Added versioned JSON dependency/package metadata to configured builds,
  install prefixes, and release archives.
- ✅ Added stable SemVer tag-driven Windows/Linux Core SDK release automation
  with project-version validation and SHA-256 checksum assets.
- ✅ Covered Vulkan/headless Debug and Release configurations in the manually
  dispatched capability workflow.

The capability workflow and its evidence contract are complete. Enrolling a
repository-scoped GPU runner for continuous execution remains in the
[backlog](../roadmap/backlog.md#cross-cutting-planned-work).

## Public project baseline ✅

- ✅ Published the Apache License 2.0 text.
- ✅ Added contribution, security, and changelog policies.
- ✅ Added build/install, CMake package, support-matrix, and renderer
  architecture documentation.
- ✅ Documented the intentionally unavailable MaterialX, advanced viewport, and
  low-copy GPU interop features for v0.1.0.

## Measurement baseline ✅

- ✅ Added `merlin-benchmark` with a versioned, fixed-order JSON schema and
  commit, build type, compiler, OS, GPU/driver, Vulkan API, and resolution
  metadata.
- ✅ Added CPU scopes for scene update, extraction, upload, command recording,
  readback, and total frame time.
- ✅ Added per-frame draw/triangle, transfer byte, allocation, pipeline, and
  scene/pipeline cache counters.
- ✅ Added first-frame, steady-state median, and scene-edit baselines with CI
  assertions for static-scene zero-upload/allocation/pipeline behavior.

## MaterialIR and basic shading ✅

- ✅ Added host-neutral material parameter blocks, texture/sampler bindings,
  feature masks, alpha/cutoff and double-sided state, plus independent resource
  revisions and structured extraction fallback records.
- ✅ Added completion-safe Vulkan texture/sampler residency, descriptor updates,
  and shader-module, descriptor-layout, and feature-keyed pipeline caches.
- ✅ Rendered base/vertex color, display opacity, normals, UV image textures,
  directional light, opaque surfaces, and alpha masks through the common
  renderer path; value-only edits reuse pipelines.
- ✅ Added a textured directional-lit headless reference scene and a basic Hydra
  `UsdPreviewSurface`/`UsdUVTexture`/distant-light translation with install-tree
  usdview evidence.

## Performance observability foundation ✅

- ✅ Split GPU scene update, command recording, queue submission, GPU timestamp
  execution, completion wait, and CPU readback; recorded selected AOVs, bytes,
  maps, resolves, descriptor work, allocation bytes, and visible primitives.
- ✅ Upgraded the renderer benchmark to versioned distribution and hitch
  summaries with static, camera, edit, AOV, million-triangle, 10,000-mesh,
  1,000-instance, and 4K fixtures.
- ✅ Added structural comparison reports with limiting-stage identification and
  optional controlled-hardware timing thresholds.
- ✅ Added Hydra Sync/fetch telemetry and combined it with OpenUSD Chrome trace
  scopes for scene-index processing, CPU-to-Hgi upload, composite, and
  presentation in a versioned install-tree report.
- ✅ Gated static and camera-only paths against irrelevant fetch, upload,
  allocation, shader, pipeline, and geometry-cache work, and retained all
  reports in capability CI.

## Incremental Hydra synchronization ✅

- ✅ Retained per-USD-path Hydra topology, points, primvar descriptors and
  values, normalized/indexed primvars, triangulation, packed geometry, material,
  transform, and visibility state across Sync calls.
- ✅ Added locator-aware points, topology, primvar, transform, visibility,
  camera, and material-parameter fast paths with conservative fallback for
  coarse or unavailable dirty information.
- ✅ Preserved distinct source and derived resource revisions plus normalized
  changed vertex/index ranges through RenderWorld, extraction, and Vulkan
  residency; compatible resident revisions receive exact partial uploads and
  incompatible shapes safely fall back to full upload.
- ✅ Added versioned host-neutral `merlin-diagnostic/v1` records with stable
  codes, source paths, dispositions, and recovery actions, bridged to Hydra
  warnings and telemetry.
- ✅ Validated removal/re-addition, malformed-topology rejection and recovery,
  per-phase cache/fetch/rebuild counts, exact changed-byte upload, and zero
  unrelated work through Core, Vulkan, and install-tree usdview regressions.
- ✅ Required and recorded the OpenUSD 26.05 shared SDK for Hydra builds and
  made Release-only MSVC SDK incompatibility fail with an actionable Debug
  diagnostic.
- ✅ Documented the standard OpenUSD `ParticleField3DGaussianSplat` through
  Hydra `particleField` ingestion boundary for the later Gaussian milestone,
  without adding a custom schema or direct PLY/SPLAT parser.

## Persistent snapshot upsert path ✅

- ✅ Replaced vector-per-revision extraction tables with immutable balanced
  storage that shares unchanged records and subtrees across snapshots.
- ✅ Made geometry, material, instance, and light upserts visit and copy only
  changed records; transform edits retain draws and visibility/material-binding
  edits rebuild only their dependent transient draw.
- ✅ Added snapshot visited/copied record, rebuilt-draw, and full-table fallback
  counters to benchmark JSON and Hydra performance events.
- ✅ Covered old-snapshot immutability and record identity sharing, including a
  localized edit in the 10,000-mesh regression fixture.

## Persistent snapshot structural edits ✅

- ✅ Replaced addition/removal full-table fallback with dense append and
  identity-preserving swap removal, updating only draws that reference removed
  or displaced geometry, material, and instance records.
- ✅ Added texture-to-material, sampler-to-material, mesh-to-instance, and
  material-to-instance reverse dependencies so structural binding invalidation
  scales with dependent records instead of the complete scene.
- ✅ Added validated snapshot-local dense indices to resource upsert deltas and
  retained handle-reconciliation compatibility for older snapshots in the
  Vulkan backend.
- ✅ Covered structural work counters, displaced record identity, immutable old
  snapshots, localized texture/sampler invalidation, and Vulkan resource and
  material regressions.

## Million-prim persistent snapshot scaling ✅

- ✅ Replaced linear free-slot scans and quadratic pending-change compaction in
  `RenderWorld` with explicit free-slot reuse and object-kind/handle indexing,
  keeping one-million-prim initial construction practical.
- ✅ Used ordered insertion hints and known dense indices during initial scene
  extraction so sorted resource, dependency, and draw construction avoids
  redundant logarithmic lookups without changing persistent table identity.
- ✅ Added a one-million-prim regression in which 100 transform edits visit and
  copy exactly 100 instance records, rebuild no draws or tables, and retain all
  unaffected record and draw identities.
- ✅ Added shared texture and sampler edits at the same one-million-prim scale;
  each visits and copies exactly its one resource record, rebuilds no draw or
  table, and retains the million instance/draw identities.
- ✅ Added 50 removals plus 50 additions at the same live prim count, covering
  displaced dense indices, identity-preserving swap removal, immutable prior
  snapshots, and bounded structural counters under the existing scale timeout.

## Descriptor-indexing negotiation ✅

- ✅ Probed every descriptor-indexing feature and sampled-image/sampler limit
  required by the planned bindless Forward path and enabled the feature chain
  only when the negotiated tables can be maintained.
- ✅ Added explicit conventional/bindless selection with versioned capability
  output and machine-readable configuration, feature, and limit fallback
  reasons.
- ✅ Covered exact limits, per-stage resource overhead, retained sampler
  allocation overhead, forced conventional configuration, and missing-feature
  fallback without requiring a GPU.

## Bindless resource-table foundation ✅

- ✅ Added finite texture and sampler slot tables with table identity,
  generation-checked handles, deterministic free-list reuse, actionable
  exhaustion errors, and current/peak/retiring/available telemetry.
- ✅ Reserved stable white, black, flat-normal, and error texture indices and
  deduplicated sampler slots by Vulkan-affecting descriptor values while
  excluding debug labels.
- ✅ Delayed slot reuse and generation advancement until the last completion
  value, coalesced dirty descriptor indices, and recorded allocation, reuse,
  update, retirement, collection, exhaustion, stale-generation, reference, and
  deduplication evidence.

## Vulkan bindless residency foundation ✅

- ✅ Connected revisioned Vulkan textures and samplers to the finite logical
  tables so unchanged resources retain indices and changed resources receive
  distinct completion-safe replacement slots.
- ✅ Materialized white, black, flat-normal, and error RGBA8 images, created a
  partially-bound update-after-bind sampled-image/sampler descriptor set, and
  rewrote only dirty elements.
- ✅ Deduplicated Vulkan sampler objects, rewrote collected descriptors to safe
  fallbacks before destroying replaced objects, and reused a slot only after
  its generation advanced at completion.
- ✅ Added bindless per-frame descriptor counters, benchmark residency evidence,
  and a validation-enabled two-frames-in-flight regression covering stable
  use, replacement, collection, fallback rewrite, reuse, and zero steady work.

## Bindless Forward activation ✅

- ✅ Added non-uniform sampled-image and sampler array shaders that consume the
  resident table indices while retaining conventional Forward as a selectable
  feature/limit/configuration fallback.
- ✅ Replaced per-material image descriptors on the bindless path with one
  global update-after-bind resource set and one persistent dynamic-material set
  per frame context, producing zero warmed static descriptor work.
- ✅ Added configurable automatic/conventional/bindless renderer selection,
  packaged both shader ABIs, and validated exact color, depth, primId, and
  instanceId parity on a textured scene.

## Persistent arena and upload telemetry ✅

- ✅ Exposed separate vertex/index arena capacity, resident/peak/free/retiring
  bytes, active/retiring ranges, block growth, completed release counts, free-
  span counts, and largest-free-span fragmentation evidence.
- ✅ Exposed persistently mapped geometry-upload-ring capacity, reservation,
  in-flight/peak bytes and regions, growth, wrap, and retired-buffer evidence.
- ✅ Split per-frame staged payload into vertex, index, and texture bytes,
  recorded aligned ring reservations and stable range reuse, and covered
  initial growth, localized edits, fragmentation recovery, and in-flight
  replacement through Vulkan and benchmark regressions.

## Asynchronous transfer and VRAM budgets ✅

- ✅ Selects a dedicated transfer family when timeline semaphores are available,
  submits geometry and texture uploads independently, and makes graphics wait
  on a separate upload timeline without changing frame completion tokens.
- ✅ Uses concurrent geometry-buffer sharing across queue families and explicit
  transfer-to-graphics ownership/layout transitions for sampled images, with a
  validated single-queue fallback.
- ✅ Probes `VK_EXT_memory_budget` heap capacity/budget/usage, applies a
  configurable renderer device-local limit before allocation, classifies both
  proactive denials and Vulkan OOM as `resource-exhausted`, and retains
  current/peak/allocation/release/exhaustion evidence.
- ✅ Extends capability and benchmark JSON with queue selection, upload timeline,
  ownership-transfer, and VRAM current/peak/capacity evidence; lifetime tests
  cover deterministic configured-limit exhaustion.
- ✅ Exposes descriptor backend/table capacity, VRAM limit, and transfer
  selection through the headless probe; the GPU capability workflow retains
  automatic/forced-conventional metadata plus an expected actionable
  bindless-capacity exhaustion log for every configured profile.
- ✅ Makes headless validation artifacts compare a forced-conventional render
  against the automatically selected bindless render when available, retaining
  exact color, depth, and primId expected/actual/diff images; capability CI now
  runs the resource-update, material, and bindless lifetime regressions too.

## Slang shader and ABI foundation ✅

- ✅ Replaced the conventional and bindless GLSL Forward sources with shared
  Slang modules while retaining the existing Vulkan descriptor negotiation and
  runtime SPIR-V entry points.
- ✅ Pinned Slang 2026.8.x and added dependency-aware SPIR-V and Metal codegen,
  per-target reflection, versioned build/install packaging, and deterministic
  cache keys with source, compiler, profile, capability, matrix-layout,
  optimization, entry-point, permutation, and generator provenance.
- ✅ Extracted stable C++ Forward push-constant, material-uniform, and resource-
  binding ABI declarations; reflected tests fail with actionable field or
  set/binding diagnostics and Core retains only semantic shader capabilities.
- ✅ Declared Metal non-uniform bindless indexing unsupported and selected the
  conventional Forward compile gate without weakening the Vulkan bindless path.
- ✅ Extended forced-conventional versus automatic-path artifacts to exact
  color, depth, primId, and instanceId comparisons, and removed the superseded
  GLSL runtime path.
- ✅ Confirmed the repository-scoped Windows x64 `vulkan-1.4` runner with a
  green Debug/Release Vulkan and OpenUSD 26.05 Hydra
  [capability run](https://github.com/animu-sphere/hydra-merlin/actions/runs/29508228337).
  Release evidence builds the `v0.7.0` tag on the same GPU: structural
  comparison passes with zero regressions, and the 120-frame steady-state GPU
  median moves from 1,231,700 ns to 1,188,085 ns (-3.5%). Raw reports and a
  non-gating 20% timing observation are retained for review.

## Backend-neutral viewport and Vulkan presentation ✅

- ✅ Added `Merlin::RenderBackend` with backend selection/factories,
  renderer-meaning capabilities and limits, logical presentation and completion
  handles, submit/resolve, common timings/counters, and structured errors.
- ✅ Adapted the existing Vulkan renderer and Hydra delegate to that contract
  while keeping Vulkan, Metal, GLFW, and native surface types out of Core and
  concrete backend/window types out of the Hydra public boundary.
- ✅ Added the permanent GLFW-hosted `merlin-viewport` product with
  usdview-style, `upAxis`-aware framing and tumble/track/dolly controls, resize,
  click-triggered ID readback, screenshots, title timing, backend selection,
  benchmark mode, and optional OpenUSD loading through Hydra.
- ✅ Added Vulkan surface/swapchain ownership, FIFO and vsync-off present-mode
  selection, GPU-only color blit into the acquired image, out-of-date/resize
  recovery, and per-swapchain-image completion semaphores.
- ✅ Retained exact viewport/offscreen color and depth parity, zero CPU readback
  on normal presentation frames, resize recreation, presentation-copy bytes,
  validation message counts, and CPU/GPU timing evidence in CTest and
  `merlin.viewport-benchmark/v1` reports.
- ✅ Added Core-only, installed-package, Vulkan viewport, and OpenUSD/Hydra USD
  viewport coverage plus a forbidden concrete-backend type scan.

## MaterialXGenSlang compiler foundation ✅

- ✅ Added the optional `material/merlin-materialx` component and exported
  `Merlin::MaterialX` target, privately linked to the pinned MaterialX 1.39.6
  MaterialXGenSlang implementation and disabled by default.
- ✅ Kept MaterialX SDK types out of the public compiler API and preserved
  Core/Vulkan builds with `MERLIN_ENABLE_MATERIALX=OFF`.
- ✅ Generated a graph-only `evaluateMaterial(MaterialInputs)` Slang function
  without a vertex stage, renderer entry point, AOV output, or lighting block
  for the initial constant, convert, add, multiply, and mix-capable slice.
- ✅ Retained generator/MaterialX provenance, logical input and uniform
  reflection, deterministic generated source, and a SHA-256 identity over the
  canonical input and source.
- ✅ Added actionable invalid-document, missing-library, renderable-selection,
  unsupported-node/output, and generation diagnostics at the integration
  boundary.
- ✅ Compiled one generated module through direct SPIR-V and Metal-target test
  wrappers with target reflection when `slangc` is available, and verified the
  optional installed-package consumer.
- ✅ Added image/UV0/world-normal generation and a renderer-owned minimum
  Standard Surface result for `base`, `base_color`, `metalness`,
  `specular_roughness`, and `normal`, while rejecting explicitly authored
  out-of-scope Standard Surface inputs.
- ✅ Kept topology, parameter, and texture-default identities separate across
  the Standard Surface slice and included portable loaded-library and
  transitive generator-source fingerprints in the module key.
- ✅ Compiled the textured Standard Surface result wrapper from one generated
  source through both SPIR-V and Metal targets without allowing resources to
  leak from the constant-buffer binding.
- ✅ Composed compiler, target, profile, capability, layout, optimization,
  debug, and target-option policy into a Core target-artifact key that keys
  generated and handwritten Slang identically, and made the `shaders/v2`
  manifest record the module sources, module identity, and artifact key that a
  test recomputes through that contract. Handwritten module sources come from
  the depfile the compiler emitted, so an added include cannot slip past the
  key.

This is the foundation stage of the boundary rather than the whole of it. The
common diagnostic/fallback contract, Vulkan Forward execution, and image
evidence that completed it are recorded in the v0.10.0 section above and
released in [v0.10.0](../releases/v0.10.0.md); the accepted
[MaterialXGenSlang policy](../design/materialxgenslang-boundary.md) remains the
authoritative ownership contract.
