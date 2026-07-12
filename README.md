# Checklist Assistant

Checklist Assistant is a local-first procedural execution framework. A checklist step is more than text: it is an instruction, a state transition, a validation point, and a relational node that can be imported, executed, evaluated, reported, and moved as a portable asset pack.

This public repository is the reference implementation source. It intentionally starts small: C++ server, static web UI, MCP bridge, tests, generic example checklists, and the public specification. Compiled release artifacts, private/customer checklist packs, local databases, and Whisper/VUI assets are not included.

## Current Scope

- C++20 HTTP server with SQLite runtime storage.
- Static browser UI under `CHAX-CLIENT/web`.
- Markdown import/export for canonical checklist folders.
- Transportable asset packs using `.chk`, `.7z`, or `.zip` archives; `.chk` is a normal 7-Zip archive with a Checklist Assistant extension.
- Relationship and predicate evaluation for deterministic checklist status updates.
- MCP bridge for agent/tool clients.
- Generic example checklists under `checklists/`, including basic edit/report flows and predicate demos.

## Build

Prerequisites:

- CMake 3.28 or newer.
- Ninja.
- A C++20 compiler. On Windows, the expected path is MinGW-w64 GCC through MSYS2 UCRT64.
- 7-Zip for asset-pack import/export. The server checks `7z` on `PATH`, `CHAX_7Z_PATH`, and common Windows install locations such as `C:\Program Files\7-Zip\7z.exe`.

From PowerShell:

```powershell
cmake --preset windows-gcc-ninja
cmake --build --preset windows-gcc-ninja
ctest --preset windows-gcc-ninja --output-on-failure
```

The Windows preset is a compact Release build and stages a portable runtime outside the repository:

```powershell
cmake --build --preset windows-gcc-ninja --target stage-windows-runtime
```

The staged runtime is written to the sibling folder `../Checklist-Assistant-runtime/`. Build and test output remains ignored under `out/` and can be deleted after staging.

## Run Locally

```powershell
..\Checklist-Assistant-runtime\checklist_assistant.exe --host 127.0.0.1 --port 8080 --no-browser
```

Then open `http://127.0.0.1:8080/ui/`.

The server creates local runtime state in `.chax/`. That folder is ignored by git and can be removed to reset local runtime data.

## Private Checklist Assets

The Windows `stage-windows-runtime` target removes and recreates the whole sibling `Checklist-Assistant-runtime` folder. Treat that runtime as disposable: do not keep the only copy of a private or customer-specific checklist under its `checklists/` folder.

Instead, keep private assets in a separate sibling workspace or repository with the same canonical shape:

```text
<private-assets>/
  checklists/
    <private-pack>/
      <checklist>/
        checklist.md
```

In the running application, open Portal Settings and add the private workspace as `source_name=path`. The setting is machine-local in the runtime's ignored `.chax/local_settings.json`; it is not copied into the public repository. Select the exact source and pack when importing, exporting, running scripts, or generating a report so private assets remain in their owning workspace. See [the private-pack guidance](docs/user_manual.md#16-private-packs-and-public-examples) for the required layout and cutover cautions.

## Repository Layout

- `src/`: C++ server, app core, platform adapters, and tools.
- `CHAX-CLIENT/web/`: static web UI served by the local server.
- `CHAX-CLIENT/mcp-voice-assistant/`: client-side prompt/helper package for MCP voice hosts.
- `checklists/`: generic public examples and the unit-test controller checklist.
- `spec/`: public Checklist Assistant specification and compatibility material.
- `docs/`: user and MCP documentation.
- `scripts/`: repository automation and API smoke helpers.
- `tests/`: C++ and browser-side tests.
- `third_party/`: vendored source dependencies with their upstream licenses.

## Licensing And Identity

The reference implementation source is MIT licensed. Specification material under `spec/` is Apache-2.0 licensed. Checklist Assistant project identity and compatibility wording are governed by `TRADEMARK.md` and `COMPATIBILITY.md`.

Private or customer-specific asset packs are not part of this public repository. Public examples should stay generic, reproducible, and free of private operational details.
