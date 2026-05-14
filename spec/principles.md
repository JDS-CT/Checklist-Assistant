# Checklist Assistant Design Principles

This note records the stable rationale behind the specification without carrying private deployment history or scratchpad prose. The specification remains the normative source for identity, API, storage, relationship, and reporting behavior.

## Offline Capabilities

Execution of SOP in isolated environments is possible, keep the capabilities on embeded, portable, secured or analogue devices (even paper if absolutly necessary). Tested to ensure compatibility with network isolated environments

## Active SOPs

- A checklist row should be both human-readable and machine-addressable: a person can perform the action, while software can update, query, relate, and report the row through the same API.
- `procedure` is the planning label; `action` is the execution cue; `spec` is the expected condition; `result`, `status`, and `comment` are the mutable observation surface.
- Long procedural knowledge belongs in `instructions`; short fields stay compact so operators, agents, and reports can scan rows quickly.

## Deterministic First

- Prefer deterministic checks before model judgment: explicit relationships, `spec`/`result` Verify grammar, unit-aware comparisons, and stable IDs should handle the cases they can handle.
- Qualitative rows should use explicit adjudication through `Pass`/`Fail` rather than pretending every observation has a numeric parser.
- LLMs and voice clients should operate through the same HTTP/MCP contracts as the web UI; no client gets a private shortcut around the runtime store.

## Portability

- Checklist asset packs are self-contained by default: `checklist.md`, optional `checklist.json`, `data/`, `media/`, `templates/`, runtime `reports/`, runtime `logs/`, and optional `scripts/`.
- The current C++ reference implementation defaults to `checklists/<pack>/<checklist>/` and can relocate the root with `CHAX_CHECKLISTS_ROOT`.
- Relative paths are preferred for media and templates so a checklist can move between local, release, and hosted environments without path rewrites.

## Relationship Graphs

- Relationships make hidden assumptions inspectable. A prerequisite, prefill, verification, or propagation edge should be visible as data rather than trapped in training, memory, or a one-off script.
- The core relationship triple stays small: source, predicate, target. Custom predicate semantics can exist at the deployment edge without changing the portable graph format.
- Automation that acts on relationships must write through the normal update contract and carry an accountable `entity_id`.

## Small-Model Compatibility

- The authoring surface should be simple enough for a small local model to follow: one canonical Markdown shape, concise fields, explicit instructions, and no invented IDs.
- Larger models can perform deeper audits and migrations, but they should leave behind compact docs and examples that smaller models can load first.
- The goal is hard-to-build but easy-to-use: sophisticated implementation behind a plain, stable checklist format.

## Configuration as Checklist

- Prefer configuration to be represented as checklist rows when the option benefits from action/spec/instructions context and auditability.
- Boolean options can map to status (`Pass` for enabled/true, `Fail` for disabled/false) when the row's spec/result make that meaning explicit.
- Deployment config files may be derived from checklist state, but the checklist should remain the self-documenting source of intent.
