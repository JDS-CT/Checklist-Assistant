# Agent Guide

## Basics

- Read `PRINCIPLES.md` before making design, data-format, or source-control decisions.
- Keep tracked text files UTF-8 without BOM.
- Use CMake and Ninja for builds; avoid adding IDE-only project files.
- Keep public examples generic and free of private/customer/vendor-specific operational content.
- Do not commit generated databases, build output, release binaries, logs, archives, or local settings.

## Validation

- Prefer `rg` for search.
- For C++ changes, run the relevant build and tests. On Windows, start with `cmake --preset windows-gcc-ninja`; build only the target under test or `stage-windows-runtime`, then run the relevant `ctest` selection. Do not use an unqualified `cmake --build --preset windows-gcc-ninja` for ordinary runtime work: it builds every test executable and produces avoidable local build churn.
- For documentation-only changes, at least run `scripts/check_text_encoding.ps1`.

## Runtime Packaging

- The normal Windows preset is a compact Release build. It stages the runnable launcher, server, MinGW DLLs, web client, and public checklists to the ignored sibling folder `../Checklist-Assistant-runtime/` through the `stage-windows-runtime` target.
- Treat `out/` as disposable compiler output. Do not commit, archive, or hand off the CMake build tree. Delete it after staging a verified runtime when disk space matters.
- Keep private packs outside the repository and runtime staging folder; load them through Portal Settings. A clean runtime folder may be removed and rebuilt without affecting private packs.

## Public Safety

- The public repo is local-first. Do not describe hosted or shared-LAN deployment as production-ready until `SECURITY.md` and TODO follow-up items have been resolved.
- Do not add private asset packs. Use sibling local folders for private packs and import them through Portal Settings.
