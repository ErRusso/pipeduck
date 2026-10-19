# Security Policy

## Supported versions

| Version | Supported |
| --- | --- |
| 1.x | Yes |

## Reporting a vulnerability

Please do not open a public issue for security problems. Report them privately through
[GitHub Security Advisories](https://github.com/ErRusso/pipeduck/security/advisories/new) or email
**s.russo06@icloud.com**.

Include:

- A description of the issue and its impact.
- Steps to reproduce, if possible.
- The version, distribution and PipeWire version.
- Any suggested mitigation.

You can expect an acknowledgement within a few days. Once a fix is available, a release will be
published and the advisory will be disclosed with credit unless you prefer to remain anonymous.

## Scope

PipeDuck controls the local PipeWire/PulseAudio graph through `pactl`, `pw-link` and libpulse. Issues
that are especially relevant include:

- Unintended movement or termination of unrelated audio streams.
- Command or property injection through stream names or configuration values.
- Unsafe handling of the D-Bus tray interface.
- Privilege escalation (PipeDuck intentionally runs as the desktop user and requires no root).

Issues in PipeWire, WirePlumber, PulseAudio, GTK or the desktop tray host should be reported to the
respective projects.
