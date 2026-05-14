# Security

Checklist Assistant's first public release is local-first. The recommended default is binding the server to `127.0.0.1` and using it on the same workstation.

## Supported Initial Use

- Local development and local checklist execution.
- Local browser UI served by the C++ server.
- Local MCP or script clients that connect to the same machine.

## Not Yet Recommended

- Internet-facing hosting.
- Shared-LAN hosting for untrusted users.
- Storing secrets in checklist packs, scripts, prompts, or MCP configuration.

Hosted/LAN hardening is tracked in `TODO.md`. Before broader deployment, the project needs sharper guidance for TLS termination, authentication policy, CSRF/CORS boundaries, audit logging, backup/restore, and checklist-pack trust.

## Reporting Issues

For now, open a GitHub issue with a clear reproduction. Do not include private checklist packs, credentials, logs with secrets, or customer material.
