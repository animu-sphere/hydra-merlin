# Reports

Dated ad hoc implementation evidence remains local-only by default. The
repository tracks this README, the curated [delivery history](delivery-history.md),
the reviewed, append-only [OST dogfooding series](ost/README.md), and the
[release-engineering review series](releases/README.md). Other files under this
directory remain Git-ignored so raw machine output does not become public
documentation accidentally.

Reports capture *how* a change was validated at a specific time. They are
evidence and upstream feedback, not a published product contract:

- Design rationale that outlives a single session belongs in [design](../design/).
- Shipped scope belongs in the [changelog](../../CHANGELOG.md), with optional
  deep-dive [release records](../releases/).
- Incomplete work belongs in the [roadmap](../roadmap/).
- Completed pre-release roadmap detail belongs in the
  [delivery history](delivery-history.md).
- Current support belongs in the
  [support matrix](../reference/support-matrix.md).

Each report or delivery-history entry makes its point-in-time nature obvious by
stating its date and, where relevant, the commit, platform, hardware and
configuration. Reports are not the canonical description of current support:
current documents cite them as "evidence captured for this claim is available
in ...", never as "current support is defined by ...". See the
[documentation ownership rules](../README.md#ownership-rules).
