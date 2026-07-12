# Security Policy

## Supported versions

hdMerlin has not published its first release. Until v0.1.0 ships, `main` is a
development snapshot supported on a best-effort basis. After releases begin,
security fixes will land on `main` and in the latest release; no long-term
support branches are currently planned.

| Version | Supported |
| --- | --- |
| `main` before v0.1.0 | Best effort |
| latest release | Yes |
| older releases | No; upgrade to latest |

## Reporting a vulnerability

Please report suspected vulnerabilities privately rather than opening a public
issue. Use GitHub's private
[Security Advisories](https://github.com/animu-sphere/hydra-merlin/security/advisories/new)
for this repository.

Include, where possible:

- the affected version or commit;
- operating system, compiler, GPU/driver, Vulkan version, and OpenUSD version as
  applicable;
- a description of the issue and its impact;
- reproduction steps or a proof of concept;
- any suggested remediation.

Do not include third-party secrets or live credentials in a report.

## What to expect

This is a small project maintained on a best-effort basis. We aim to acknowledge
a report within a few business days, assess its severity, keep the reporter
updated while a fix is developed, and coordinate disclosure after a fix is
available. Please allow reasonable time for investigation and release.

## Scope

In scope are hdMerlin's Core libraries, Vulkan backend, headless executable,
Hydra adapter, shader and package loading, CI/release automation, and installed
CMake package contract.

Vulnerabilities in Vulkan, OpenUSD, CMake, compilers, GPU drivers, or other
third-party dependencies should be reported to their upstream maintainers. A
private heads-up is still welcome if hdMerlin's use of a dependency materially
worsens the issue.
