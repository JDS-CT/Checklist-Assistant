# Maintainer Handoff

This repository was seeded as a public-safe starting point from an internal development tree. It is ready for review before the first public commit, but it has intentionally not been committed by the seeding pass.

## What Was Included

- C++ source under `src/`.
- Static web UI under `CHAX-CLIENT/web/`.
- MCP voice-helper client package under `CHAX-CLIENT/mcp-voice-assistant/`.
- Generic public checklists under `checklists/chax`, `checklists/examples`, and `checklists/unit-tests`.
- Test-only fixtures under `tests/fixtures`.
- Tests under `tests/`.
- Public docs under `docs/`.
- Public specification material under `spec/`.
- Required third-party source dependencies under `third_party/cpp-httplib`, `third_party/nlohmann`, `third_party/sqlite`, and `third_party/xxhash`.

## What Was Excluded

- Compiled release artifacts and packaged release folders.
- Runtime databases, logs, generated reports, and local settings.
- Private/customer checklist packs.
- Large private/media assets and transport archives.
- Whisper/VUI client assets. The current server still has optional code paths for a VUI root if one is provided later, but the public seed does not ship that client.
- Internal planning material, memos, research notes, and public-repo-transition notes.

## Suggested First Review

1. Run `git status -sb` and review all untracked files.
2. Run `scripts/check_text_encoding.ps1`.
3. Configure/build/test from this repository.
4. Run a final public-safety scan for private names, archive files, binaries, generated output, and local state.
5. Make the first commit only after that review is clean.

## Windows Runtime Workflow

The normal Windows preset is intentionally a compact Release build. Configure with `cmake --preset windows-gcc-ninja`, then use `cmake --build --preset windows-gcc-ninja --target stage-windows-runtime` to stage a portable runtime in the ignored sibling folder `../Checklist-Assistant-runtime/`.

Do not use an unqualified preset build as the normal launch path: it builds every test executable and can make local compiler output much larger than the runtime. The ignored `out/` tree is disposable; verify the staged runtime, then remove `out/` when it is no longer needed. Keep private checklist packs outside both the repository and runtime folder and load them through Portal Settings.

## Licensing Context

- Root implementation code is MIT licensed through `LICENSE`.
- Specification material under `spec/` is Apache-2.0 licensed through `spec/LICENSE`.
- Project identity and compatibility language are documented in `TRADEMARK.md` and `COMPATIBILITY.md`.

## Security Context

The initial public release is local-first. Hosted/LAN security is a follow-up tracked in `TODO.md` and `SECURITY.md`.
