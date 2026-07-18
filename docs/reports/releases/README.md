# Release engineering reports

These append-only reports record release-pipeline reviews, local simulations,
and publication evidence. They are distinct from the product
[release records](../../releases/), which describe shipped user-facing scope.

| # | Date | Report | Focus |
| --- | --- | --- | --- |
| 1 | 2026-07-12 | [v0.1.0 release-foundation review](01-2026-07-12-v0.1.0-release-foundation-review.md) | Tag-driven workflow, installed release metadata, verification scripts, and first-tag risks |

Only this README and reports named `<sequence>-YYYY-MM-DD-<focus>.md` are
eligible for Git tracking. Raw workflow output, temporary simulation files,
archives, checksums, credentials, environment dumps, and machine-specific
paths remain local. Before commit, reports are checked for secrets, absolute
user paths, broken relative links, and whitespace errors.
