# Security Policy

## Supported Versions

Only the latest published release receives security fixes.

| Version | Supported |
|---------|-----------|
| 1.1.x   | yes       |
| 1.0.x   | no        |
| < 1.0   | no        |

## Reporting a Vulnerability

Please report suspected vulnerabilities **privately** via GitHub's security
advisories:

1. Go to <https://github.com/paolostivanin/blk-monitor/security/advisories>
2. Click "Report a vulnerability"
3. Provide a clear description, reproduction steps, and the affected version.

Please **do not** open a public issue for security reports.

You can expect an initial acknowledgement within 7 days. Confirmed issues are
typically patched and released within 30 days of confirmation, sooner for
high-severity findings.

## Scope

`blk_monitor` is a small Linux CLI that reads `/sys/block/<dev>/stat` and calls
`sync(2)`. In-scope concerns include, for example:

- Path-traversal or symlink attacks via the user-supplied device path
- Memory-safety issues (overflow, OOB read/write, use-after-free, double-free)
- Format-string or integer-conversion issues
- Privilege-escalation paths when the binary is run with `sudo` or
  `cap_sys_admin+ep`
- Signal-handling bugs that could leak state or hang the terminal

Out of scope:

- Findings that require an attacker with root or with arbitrary write access to
  `/sys/block` (kernel APIs are trusted)
- Behavior on kernels older than 2.6.26 (not supported)
- Hardening recommendations without a concrete attack scenario
