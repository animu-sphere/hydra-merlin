# Documentation

This directory holds every hdMerlin document beyond the repository
[README](../README.md). Each document class has one responsibility, and each
frequently changing fact has exactly one canonical home. Other documents link
to that home instead of copying its state.

## Map

| Directory | Answers | Start with |
| --- | --- | --- |
| [design/](design/) | What are the durable contracts, boundaries and algorithms? | [Renderer architecture](design/renderer-architecture.md) |
| [guides/](guides/) | How do I build, run, validate, benchmark or release it? | [Build and install](guides/build-and-install.md) |
| [reference/](reference/) | What works now, and with what evidence? | [Support matrix](reference/support-matrix.md) |
| [roadmap/](roadmap/) | What is being worked on now, and what may come later? | [Current](roadmap/current.md) |
| [releases/](releases/) | Why was a release considered complete? | [Release records](releases/README.md) |
| [reports/](reports/) | What was measured or investigated at a point in time? | [Reports](reports/README.md) |

Shipped user-visible changes are recorded in the [changelog](../CHANGELOG.md).

### Design

- [Renderer architecture](design/renderer-architecture.md)
- [Execution and render-product lifetime](design/execution-lifetime.md)
- [Renderer settings](design/renderer-settings.md)
- [Forward lighting](design/forward-lighting.md)
- [Surface geometry, normals and tangent frames](design/surface-geometry.md)
- [MaterialXGenSlang material boundary](design/materialxgenslang-boundary.md)
- [Multi-backend shader and presentation strategy](design/multibackend-slang-materialx.md)
- [Hgi host presentation](design/hgi-host-presentation.md)
- [GPU-driven rendering](design/gpu-driven-rendering.md)
- [Gaussian rendering](design/gaussian-rendering.md)
- [Gaussian ingestion through Hydra](design/gaussian-hydra-ingestion.md)
- [Metal Gaussian execution](design/metal-gaussian-execution.md)
- [OpenStrata project layout](design/openstrata-project.md)

### Guides

- [Build and install](guides/build-and-install.md)
- [Running the viewport and Hydra hosts](guides/viewport.md)
- [Forward validation](guides/forward-validation.md)
- [Benchmarking](guides/benchmarking.md)
- [Using the CMake package](guides/cmake-package.md)
- [Releasing](guides/releasing.md)

## Source-of-truth map

| Information | Canonical source |
| --- | --- |
| Project identity and elevator pitch | [README.md](../README.md) |
| Architecture and ownership decisions | `docs/design/*` |
| How to build, run, debug and validate | `docs/guides/*` |
| Current supported capability and evidence level | [support-matrix.md](reference/support-matrix.md) |
| Current active work | [roadmap/current.md](roadmap/current.md) |
| Future and deferred work | [roadmap/backlog.md](roadmap/backlog.md) |
| Shipped user-visible changes | [CHANGELOG.md](../CHANGELOG.md) |
| Release-specific closure and evidence | `docs/releases/vX.Y.Z.md` |
| Investigation, benchmark and historical evidence | `docs/reports/*` |
| Exact dependency pins | [`cmake/MerlinVersions.cmake`](../cmake/MerlinVersions.cmake) and other build configuration |
| Project version | [`VERSION`](../VERSION) |
| CI-enforced behavior | Tests and [workflow configuration](../.github/workflows/) |
| Public API contract | Public headers plus the relevant design or reference document |

## Ownership rules

1. **README is the front door, not the building.** It answers what hdMerlin
   is, why it exists, its major capabilities, the shortest path to build or
   run it, and where to read next. Detailed procedures, support state and
   roadmap detail live under `docs/`.
2. **The support matrix says what works now.** It owns every current support
   claim, including platform, backend, SDK, host, feature coverage, evidence
   level and known limitations. It contains no roadmap promises.
3. **`current.md` says what is being worked on now.** It holds only active
   work, immediately next work and the current milestone's exit conditions.
   Completed tasks leave it; it is never a historical log.
4. **The backlog says what may come later.** It is organized by milestone and
   does not duplicate active task detail from `current.md`.
5. **The changelog says what shipped.** It excludes unfinished work, the active
   roadmap, detailed validation methodology and architecture discussion.
6. **Release records explain why a release was closed:** theme, completed
   scope, behavioral changes, an evidence summary, known limits and links to
   canonical documents. They are not a second support matrix.
7. **Design documents define durable contracts:** responsibility boundaries,
   invariants, algorithms, data flow, API/ABI ownership, fallback behavior and
   accepted tradeoffs. They avoid completion percentages, release targets and
   "currently being implemented" prose, and link to the support matrix for
   current implementation limits.
8. **Guides explain procedures.** They do not maintain support tables; they
   link to the support matrix for validated platforms and SDK versions.
9. **Reports preserve point-in-time evidence.** Each states its date, commit,
   platform, hardware and configuration. Current documents cite a report as
   "evidence captured for this claim", never as the definition of current
   support.
10. **Replace duplication with links.** Do not synchronize duplicated prose.
    Prefer facts derived from code, build configuration or `--help` over
    handwritten inventories.

Links point from entry documents toward canonical ones:

```text
README -> guides / design / reference / roadmap
roadmap -> design / reference
guides -> reference / design
release records -> design / reference / reports
reports -> (optional implementation context)
```

A design document must not need the roadmap to explain its architecture, and
the support matrix must not use release records or reports as its primary
definition.

## Support vocabulary

The support matrix uses these terms:

| Term | Meaning |
| --- | --- |
| Supported | Available as part of the public contract and exercised by required (per-commit or release) tests. |
| Validated | Available; execution evidence exists for the listed hardware, SDK and host configurations, but is not continuously gated on every configuration. |
| Experimental | Opt-in; behavior, settings or evidence may change. |
| Unsupported | Not available; inputs produce an explicit diagnostic or fallback. |
| Not yet validated | May build or appear to work, but no execution evidence supports a claim. |

Claims distinguish analytic correctness, small deterministic fixtures,
representative real scenes, native backend execution, Hydra host execution,
representative hardware and performance. A passing focused fixture does not by
itself make a feature broadly supported.

## Documentation lifecycle

Every pull request that changes behavior answers three questions:

1. **Did current support change?** Update the
   [support matrix](reference/support-matrix.md).
2. **Did shipped user-visible behavior change?** Update the
   [changelog](../CHANGELOG.md); release preparation also updates the release
   record.
3. **Did architecture or usage change?** Update the relevant design document or
   guide.

Do not update unrelated status documents merely to mention the same change
again. When a roadmap task ships:

```text
current.md          remove the completed task
CHANGELOG.md        record the user-visible result
support-matrix.md   update current capability, if applicable
design/             update only if the contract changed
guides/             update only if the procedure changed
release record      capture closure and evidence when releasing
```

`scripts/check-docs.py` runs in CI. It verifies repository-relative Markdown
links and anchors and rejects completed `[x]` items in
[roadmap/current.md](roadmap/current.md). Run it locally with
`python scripts/check-docs.py`.

## Review checklist

Use this for large documentation changes and releases.

- **README:** Is each section needed for first-time orientation? Are
  procedures in a guide, capability state in the support matrix, and roadmap
  detail linked rather than copied? Is release history omitted?
- **Roadmap:** Does `current.md` hold only active and immediately next work,
  with completed tasks removed? Does the backlog avoid duplicating it? Are
  future milestones separate from current support?
- **Design:** Does the document describe a durable contract without transient
  completion statements? Are capability claims delegated to the support matrix?
- **Reference:** Is there one canonical current support statement, backed by
  evidence appropriate to the claim, without roadmap promises?
- **Releases:** Does the record explain why the release closes without becoming
  another permanent support table?
