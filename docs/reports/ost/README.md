# OST dogfooding reports

This repository is exercised with
[OpenStrata](https://github.com/animu-sphere/open-strata) (`ost`). These dated
reports record what was actually tested, what worked, and which concrete
upstream outcomes would improve renderer development. The organization follows
the append-only reporting practice used by the
[usd-vrm-plugins OST series](https://github.com/animu-sphere/usd-vrm-plugins/tree/main/docs/reports/ost).

Reports are append-only historical evidence. A later result does not rewrite an
older conclusion; the newer report records the recheck and may add a short
forward note to the superseded report. Open work belongs in the
[roadmap](../../roadmap/), and shipped product scope belongs in the
[release records](../../releases/).

## Reading order

The newest report carries the live ask list. Report 7 records the v0.18.0
recheck and carries the open v0.19.0 asks.

| # | Date | Report | `ost` | Focus |
| --- | --- | --- | --- | --- |
| 7 | 2026-07-18 | [v0.18.0 ask recheck](07-2026-07-18-v0.18.0-recheck-v0.19.0-asks.md) | 0.18.0 | Completion-bound evidence and capability-scoped advice confirmed; `renderer viewport` exercised; external provenance, producer-session schema, and build intents open |
| 6 | 2026-07-18 | [MaterialX feature-intent dogfooding](06-2026-07-18-v0.17.0-materialx-v0.18.0-asks.md) | 0.17.0 | Optional MaterialXGenSlang build intent, honest external validation, and public-report redaction |
| 5 | 2026-07-15 | [Renderer dogfooding handoff](05-2026-07-15-v0.17.0-dogfooding-v0.18.0-asks.md) | 0.17.0 | Producer-session completion, target ownership, managed tests, and external provenance |
| 4 | 2026-07-13 | [Renderer adoption](04-2026-07-13-v0.16.0-renderer-adoption-v0.17.0-asks.md) | 0.16.0 | Existing-renderer adoption, build completion, and generator recovery |
| 3 | 2026-07-12 | [Capability CI evidence](03-2026-07-12-capability-ci-evidence.md) | pre-adoption | Hosted/core and capability-runner evidence boundaries |
| 2 | 2026-07-12 | [Vulkan 1.4 baseline](02-2026-07-12-vulkan-1.4-baseline.md) | pre-adoption | Minimum-version unification, runtime checks, and validation filtering |
| 1 | 2026-07-11 | [Renderer-template foundation](01-2026-07-11-renderer-template-foundation.md) | pre-adoption | Core/backend/validation/Hydra template boundaries |

## Publication policy

Only Markdown files directly under this directory are eligible for Git
tracking. Raw JSON, logs, images, archives, build trees, and nested evidence
directories remain ignored.

Before a report is committed:

- use the filename form `<sequence>-YYYY-MM-DD-<focus>.md`;
- replace user-profile, runtime-store, source-root, scene-library, and tool
  installation paths with repo-relative paths or descriptive placeholders;
- do not paste environment dumps, credentials, authorization headers, private
  URLs, signed URLs, or private-key material;
- retain public repository URLs, public revisions, artifact digests, and
  non-identifying capability data only when they are necessary evidence;
- verify every relative link and run the repository's secret/path audit plus
  `git diff --check`.

The 2026-07-18 tracking audit found no credential, token, private-key, or
private-URL material in reports 1–5. Before the directory became tracked, one
user-profile scene path was replaced with `<scene-root>`, one broken relative
roadmap link was corrected, filenames were normalized, and the previously
omitted capability report was added to this index.
