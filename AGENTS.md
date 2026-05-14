# Agent Guide

## Basics

- Keep tracked text files UTF-8 without BOM.
- Use CMake and Ninja for builds; avoid adding IDE-only project files.
- Keep public examples generic and free of private/customer/vendor-specific operational content.
- Do not commit generated databases, build output, release binaries, logs, archives, or local settings.

## Validation

- Prefer `rg` for search.
- For C++ changes, run the relevant build and tests. On Windows, start with `cmake --preset windows-gcc-ninja`, `cmake --build --preset windows-gcc-ninja`, and `ctest --preset windows-gcc-ninja --output-on-failure`.
- For documentation-only changes, at least run `scripts/check_text_encoding.ps1`.

## Public Safety

- The public repo is local-first. Do not describe hosted or shared-LAN deployment as production-ready until `SECURITY.md` and TODO follow-up items have been resolved.
- Do not add private asset packs. Use sibling local folders for private packs and import them through Portal Settings.
