---
- Version: 0.0.2-1.1.1
- Date: 2026-05-13T12:00:00-05:00
- Project: Checklist Assistant
- Repository: https://github.com/JDS-CT/checklist-assistant.git
- Code-License: MIT (reference implementation code)
- Specification-License: Apache License 2.0 (public specification text and compatibility model)
- Status: Draft public specification
---

# Checklist Assistant Specification (CHAX)

This document defines the canonical specification for:

- Checklist slugs
- Slug IDs and Instance IDs
- Address tuples and Address IDs
- The canonical data model (addressing, state, instructions, relationships)
- The Markdown authoring format
- The SQLite runtime storage model
- Reference API and JSON/JSONL encodings

The conceptual model is implementation-neutral. The current reference implementation is a small C++ server using:

- Markdown for human authoring
- SQLite for runtime state storage
- HTTP+JSON/JSONL (and MCP) for transport

Other views (HTML forms, reports, dashboards, agent tools, voice tools, etc.) are derived from the canonical model and the runtime store.

---

## License Summary

The Checklist Assistant reference implementation code is licensed under the MIT License.

This public specification, including the canonical data model, compatibility rules, encodings, and interface descriptions, is licensed under the Apache License, Version 2.0, January 2004. The Apache License text is included in this folder.

The "Checklist Assistant" name and related project identity marks are not licensed by MIT or Apache 2.0. Use of the project name is governed separately by the trademark and project identity policy.

---

## Table of Contents

- [1. Scope and Purpose](#1-scope-and-purpose)
  - [1.1 Document Layers and Context Loading](#11-document-layers-and-context-loading)
- [2. Terminology](#2-terminology)
  - [2.1 Core Checklist Concepts](#21-core-checklist-concepts)
  - [2.2 Slug and Instance Identity](#22-slug-and-instance-identity)
  - [2.3 Addressing](#23-addressing)
  - [2.4 State, Entities, and Relationships](#24-state-entities-and-relationships)
  - [2.5 Storage and Runtime Terms](#25-storage-and-runtime-terms)
- [3. Canonical Data Model](#3-canonical-data-model)
  - [3.1 Template Fields](#31-template-fields-immutable-per-slug-id)
  - [3.2 Identity Fields and Addressing](#32-identity-fields-and-addressing)
  - [3.3 State Fields](#33-state-fields-mutable-per-address)
  - [3.4 Relationships](#34-relationships-template-and-address-graphs)
  - [3.5 Canonical Slug Shape](#35-canonical-slug-shape-conceptual-object)
- [4. Canonical Encoding and Normalization](#4-canonical-encoding-and-normalization)
- [5. Markdown Authoring Specification](#5-markdown-authoring-specification-canonical-at-authoring-time)
  - [5.1 File-Level Structure](#51-file-level-structure)
  - [5.5 Procedure Blocks](#55-procedure-blocks-level-3)
  - [5.7 Relationships Subsection](#57-relationships-subsection-level-4)
  - [5.10 Minimal Example](#510-minimal-example)
- [6. SQLite Storage Specification](#6-sqlite-storage-specification-db-storage-is-canonical-at-runtime)
- [7. Transport Interfaces](#7-transport-interfaces-http-api-json-rpc-mcp)
  - [7.4 REST API: Slugs](#74-rest-api-slugs)
  - [7.5 REST API: Relationships](#75-rest-api-relationships)
  - [7.6 History, Instances, and Workspace Discovery](#76-rest-api-history-and-instance-catalog)
  - [7.7 Evaluation](#77-rest-api-evaluation-read-only)
  - [7.9 MCP Tools](#79-mcp-tools)
- [8. Relationship Model](#8-relationship-model)
- [9. Relationship Predicate Semantics and Evaluation Model](#9-relationship-predicate-semantics-and-evaluation-model)
  - [9.5 Canonical Predicate Grammar and Layer A Semantics](#95-relationship-predicate-semantics-and-evaluation-layer-a)
  - [9.10 Evaluation Interfaces](#910-evaluation-interfaces)
- [10. Write Contracts](#10-write-contracts-creation-minimal-update-batch-ingest)
- [11. Identifier System](#11-identifier-system-canonical-id-model)
- [12. Versioning, Template Evolution, and Compatibility](#12-versioning-template-evolution-and-compatibility)
- [13. Reporting and Deployment Flags](#13-reporting-and-deployment-flags)
  - [13.5 Template Variables and Escaping](#135-template-variables-and-escaping)
  - [13.9 Checklist-local Scripts and Automation Logs](#139-checklist-local-scripts-and-automation-logs-reference-implementation)
  - [13.10 Checklist Asset Pack Layout](#1310-checklist-asset-pack-layout-reference-implementation)
- [14. Resolved Direction and Remaining Follow-up](#14-resolved-direction-and-remaining-follow-up)

---

## 1. Scope and Purpose

Checklist Assistant checklists are intended to be **active SOPs**: procedures that are authored, executed, updated, and reviewed continuously, rather than static documents that drift out of sync with real work. A row may describe physical work, software work, review work, or an automation boundary. The purpose is to give each operational action a stable digital address so human, machine, and agent workflows can exchange state without relying on hidden context.

The system described in this specification is formally named Checklist Assistant, reflecting its purpose as a structured, execution-oriented environment for authoring, instantiating, evaluating, and maintaining procedural checklists. The concise technical identifier CHAX (Checklist Assistant eXecution) is used for namespaces, MCP tool names, command-line utilities, and integration surfaces. User-facing text should prefer the full name Checklist Assistant unless a compact technical prefix is required.

In typical project management practice (WBS, task trackers, etc.), work is described with **noun-like labels** (e.g., “Software installation,” “Pressure check”) optimized for planning, reporting, and coordination. Operator-facing SOPs, training videos, and tacit know-how tend instead to use **verb-like instructions** (“Install software,” “Measure pressure”). Checklist Assistant checklists deliberately carry both views at once: each checklist slug has a **procedure** (noun phrase) and an **action** (verb phrase), allowing the same row to participate simultaneously in management structures (WBS, dashboards) and in execution tools (UIs, automations, agents). The goal is to reduce drift between administrative abstractions and actionable implementation by giving both sides a stable shared addressable unit.

This specification defines:

1. **The data model of a checklist slug**, including:
   - immutable template fields (checklist, section, procedure, action, spec, instructions)
   - deterministic **Slug ID** (template identity)
   - deterministic **Instance ID** (deployment/asset identity)
   - **address tuple** `(slug_id, instance_id)` as canonical runtime identity
   - optional composite **Address ID** (32-character Base32, slug_id concatenated with instance_id)
   - mutable state fields (result, status, comment)
   - runtime metadata (timestamp, entity_id)
   - relationships to other slugs

2. **Representations:**
   - **Markdown** as the human authoring format (client-side).
   - **SQLite** (or another database implementation) as the canonical runtime store (server-side).

3. **Reference encodings and interfaces:**
   - A minimal API update contract:
     - conceptually: `(slug_id, instance_id) + field(s) to update`
     - convenience: a single `address_id` token may stand in for the tuple
   - Example JSON/JSONL payloads for transport
   - A relationship model for dependency graphs between procedures

Operationally:

- Stakeholders author or revise checklists in Markdown using a standard template.
- In the reference implementation, a client-side parser ingests Markdown into the canonical data model and persists it via API calls into SQLite. Direct writes to SQLite are reserved for migration and maintenance tools, not for normal clients.
- Runtime systems (CLI, UI, automations, MCP tools) interact with a server that exposes the state via an API backed by SQLite.
- All client tools must, at minimum, support reading and writing checklist slugs through the API; they should not reach around the API to mutate the runtime store directly.
- At runtime, the minimal information required to update a procedure state is a **runtime address** plus one or more state fields:
  - either the address tuple `(slug_id, instance_id)`, or
  - a single composite `address_id` that reversibly encodes that tuple.

**Invariant:** The runtime MUST regenerate `timestamp` and `entity_id` from the current execution context.

### 1.1 Document Layers and Context Loading

This document is intentionally monolithic so a capable model or human reviewer can load the full normative surface when needed. Smaller models and quick authoring workflows should load narrower documents first:

- `docs/user_manual.md` for quick start, GUI usage, human authoring, small-model authoring, API/MCP operation, reports, scripts, and testing.
- `checklists/examples/checklist_authoring/data/prompt-message-pair.jsonl` for the smallest machine-loadable authoring context.
- `docs/mcp_tools.md` for the currently exposed MCP bridge tool catalog and schemas.
- This specification for identity, API, relationship, report, and compatibility edge cases; use the linked table of contents above for targeted loading.

The specification separates the **portable model** from the **reference C++ implementation**. Normative requirements use MUST/SHOULD/MAY language. Reference implementation notes describe the current C++ server behavior and should not be treated as the only possible implementation.

The specification is written to allow:

- **Provisional rows** (new procedures added by front-line users) that are immediately executable, and can later be reviewed/edited without breaking identity for existing rows.
- **Deterministic updates**: given a stable slug template and instance principal, the same logical row always maps back to the same `(slug_id, instance_id)` pair.
- **Multiple instances** of the same checklist (or template) co-existing cleanly: each physical system, asset, or deployment uses its own `instance_id`, avoiding state bleed between machines while still sharing the same slug template definitions.

---

## 2. Terminology

This section defines the core terms used throughout the specification. Later sections (data model, storage, API, MCP) rely on these definitions.

### 2.1 Core Checklist Concepts

- **Checklist** A collection of related procedures grouped for a specific context (e.g., “Computer Checks,” “Maintenance Visit,” “Site Walkthrough,” “Installation”). A checklist is the primary human-visible grouping. A single checklist may be instantiated against one or more assets or deployments.

- **Checklist Section** A logical grouping of procedures within a checklist (e.g., “System Checks,” “Voltage Checks,” “Floor Plan”). Sections help organize large checklists for authors and operators. Sections are **semantically relevant** for identity in Checklist Assistant: moving a row between sections is treated as a structural change that yields a new template identity.

- **Checklist Template** The authored definition of a checklist in Markdown, independent of any specific asset or deployment.

A template contains one or more **Checklist Template Rows**, organized into sections.

Templates are instance-agnostic: they do not know which specific machine, asset, or deployment will use them.

- **Checklist Template Row** The immutable structural definition of one checklist item within a checklist template. For the reference implementation, a template row is defined by:

  - `checklist` (the checklist name/slug)
  - `section`
  - `procedure` (noun phrase)
  - `action` (verb phrase)
  - `spec` (expected target or standard)
  - `instructions` (freeform SOP text)

Template rows do **not** include mutable state (`result`, `status`, `comment`) and do **not** carry runtime metadata (`timestamp`, `entity_id`). Mutable and runtime fields may be present in null or NaN states in intermediate representations, but they are not considered part of template identity.

- **Checklist State** The mutable part of a checklist row (that is, a checklist slug) at runtime, representing execution outcomes for a specific instance:

  - `result`
  - `status`
  - `comment`
  - `timestamp` (runtime-managed)
  - `entity_id` (runtime-managed)

Checklist state always attaches to a **runtime address** (see Address Tuple below).

- **Checklist Slug** The canonical logical unit Checklist Assistant works with: one checklist template row plus its runtime state, for a particular instance. Conceptually:

  - Template fields: `checklist`, `section`, `procedure`, `action`, `spec`, `instructions`
  - Identity fields: `slug_id`, `instance_id` (see Address Tuple)
  - Mutable state: `result`, `status`, `comment`
  - Runtime metadata: `timestamp`, `entity_id`
  - Relationships: outgoing links to other slugs

In other words, a checklist slug is “this specific template row, for this specific instance, with this current state.”

### 2.2 Slug and Instance Identity

- **Slug ID** A 16-character Crockford Base32 identifier that represents the identity of a **Checklist Template Row**. Derived solely from the immutable template fields:

  - `checklist`
  - `section`
  - `procedure`
  - `action`
  - `spec`
  - `instructions`

Two template rows with identical values for all of these fields must produce the same `slug_id`. Any change to any of these fields (including `instructions`) creates a **new** slug identity and thus a new `slug_id`. `slug_id` has no embedded semantics; it is a compact, deterministic handle for the template row.

- **Checklist Instance** A stable, deployment-defined description of the specific asset, system, or context where a checklist is being applied. This is represented as an **instance principal string**, for example:

  - `machine || model=Machine_A || serial=1234`
  - `line || line_id=Furnace_2 || plant=CT`
  - `room || building=Lab || room=201`

The content and structure of the principal string are defined by the deployment; the Checklist Assistant core treats it as opaque text.

- **Instance ID** A 16-character Crockford Base32 identifier derived from the instance principal string:

  - deterministic (same principal → same `instance_id`)
  - opaque (no embedded semantics)
  - stable for the lifetime of that instance in a deployment

`instance_id` identifies **which copy** of the template we are talking about (which building, which oven, which deployment, etc.). The underlying principal string is not stored in `slugs`, but deployments MUST make it available to any client allowed to author or instantiate checklists (REST and MCP) via a catalog table or adapter; read-only clients MAY receive only `instance_id`. Deployments MUST seed a deterministic "template" principal (for example, `template||default`) so clients can reliably target the root/template instance when no instance is specified.

### 2.3 Addressing

Checklist Assistant uses two layers of “address”:

1. **Template-level identity** via `slug_id`
2. **Runtime-level identity for a specific instance** via an **Address Tuple** `(slug_id, instance_id)`

- **Address Tuple** The ordered pair:

  - `(slug_id, instance_id)`

This is the canonical runtime identity of “one checklist slug for one checklist instance.” All state and history operations conceptually target this address tuple.

Examples:

  - “Row X of the maintenance checklist for system serial 1234”
  - “Row Y of the installation checklist for Room 201”

- **Address ID (Composite Token)** An optional 32-character Crockford Base32 identifier used as a convenience container for the address tuple. Defined as the exact concatenation:

  - first 16 characters: `slug_id`
  - last 16 characters: `instance_id`

Formally:

  - `address_id = slug_id || instance_id`

Properties:

  - fully reversible: given a valid 32-character `address_id`, the runtime can recover `slug_id` and `instance_id` by splitting it in the middle
  - human/CLI-friendly: single token for logs, URLs, labels, “hello world” examples
  - optional: the canonical key remains the address tuple `(slug_id, instance_id)`; `address_id` is a derived convenience

APIs and tools MAY allow clients to use only `address_id` in simple update calls; internal logic MUST still treat `(slug_id, instance_id)` as the true identity.

### 2.4 State, Entities, and Relationships

- **State Fields** Mutable fields and runtime metadata that represent the current outcome of a checklist slug for a given address:

  - `result` — concise observed outcome
  - `status` — enumerated outcome (`Pass`, `Fail`, `NA`, `Other`)
  - `comment` — freeform clarifying text
  - `timestamp` — ISO 8601 UTC, last update, runtime-managed
  - `entity_id` — 16-character Base32 identifier of the actor that last updated the slug’s state, runtime-managed

- **Entity** Any actor that mutates slug state (human, automation, background job, or agent). Entities are identified by an `entity_id` and may have additional metadata in a separate catalog.

- **Entity ID** A 16-character Crockford Base32 identifier for the entity that last updated a slug’s state. It is:

  - opaque (no embedded semantics)
  - stable per entity within a deployment
  - set by the runtime from authentication or system context
  - not authored in Markdown and not supplied by clients in minimal update calls

Clients obtain the active `entity_id` from the server (auth/session metadata or a discovery endpoint); the runtime echoes it in responses and history.

- **Instructions** Freeform text providing detailed guidance, SOP steps, or references associated with a template row. Instructions are treated as part of template identity (they are included in the `slug_id` computation), so changing instructions creates a new slug identity. This ensures that old instructions and new instructions cannot silently share the same identity.

- **Semantic Relationship** A directed triple describing how one checklist address or template slug relates to another:

  - `(subject_address_id, predicate_token, target_address_id)`
  - `(subject_slug_id, predicate_token, target_slug_id)`

Address-level relationships are the primary operational form and are evaluated directly in runtime address space. Template-level relationships are authored in terms of `slug_id` and are typically projected into instance context (often “same-instance”) by clients/daemons as needed.

- **Relationship Predicate Token** (`predicate_token`) An opaque, case-sensitive ASCII token stored with each relationship triple. The server stores and returns it as-is and does not interpret predicate semantics; semantic interpretation is performed by clients/daemons operating against the API (Section 9.5).

### 2.5 Storage and Runtime Terms

These terms are used when discussing the reference SQLite implementation and connectors. They do not change the conceptual model above; they are implementation details that optimize storage and indexing.

- **Normalized ID Columns** (`checklist_id`, `section_id`, `procedure_id`, `action_id`, `spec_id`, `instructions_id`) Internal database identifiers that reference deduplicated lookup tables for commonly repeated text (e.g., many rows may share the same spec text “5 V”). These IDs:

  - are not exposed in the canonical API/MCP surface
  - are not part of the conceptual identity model
  - exist solely to reduce storage duplication and improve performance

- **Runtime Store** The SQLite database that holds all slugs (template plus state), relationships, and history for a given deployment. The runtime store is the canonical source of truth for operational state.

- **Connector** Any export/import mechanism that maps between the runtime store and external systems (JSON/JSONL exports, JSON-LD, RDF triples, etc.).

- **Predicate / Aggregation Daemons (Reference Implementation)** External logic that consumes current slug state and relationship triples to (a) apply deterministic relationship rules (Layer A) and (b) compute higher-level aggregation signals (Layer B). Reference daemons operate as normal API clients identified by `entity_id` and MUST NOT write directly to the database (Section 9.5).

---

## 3. Canonical Data Model

This section defines what a checklist slug *is*, independent of any particular storage or transport format. Section 2 defined the concepts; this section fixes the exact field sets and how identity, state, and relationships fit together.

A **checklist slug** at runtime consists of:

1. Template fields (immutable within a slug)
2. Identity fields (`slug_id`, `instance_id`)
3. Addressing (`(slug_id, instance_id)` and optional `address_id`)
4. State fields (mutable)
5. Relationships (template-level graph)
6. Runtime metadata (`timestamp`, `entity_id`)

---

### 3.1 Template Fields (Immutable Per Slug ID)

Template fields define *what* a row represents, independent of the instance or current state.

#### `checklist`
Stable identifier for the checklist as a whole. Checklist names must:
- Use characters valid on Windows filesystems
- Remain stable across versions
- Match or derive from the filename when possible
- As an alternative, derive from the front-matter

A checklist may include a front-matter metadata block (author, version, date, organization) to maintain versioning independently of the filesystem. A recommended form is:

```yaml
---
Filename: checklist template
Version: 0.0.1-0.5.6
Date: 2025-11-29T15:03:00Z
Organization: Example Organization
Project: Checklist Assistant
---
```

This metadata does not participate in slug identity unless explicitly included in other template fields.

#### `section`
Logical grouping within the checklist. Moving a row between sections constitutes a structural change and therefore yields a new **Slug ID**. Examples: `Water Cooling System`, `Oven Temperature Stability`, `Floor Plan`.

#### `procedure`
Short noun phrase naming the work item.

- Used for WBS-like structures and higher-level planning
- Should be concise; default soft limit: ~32 characters
- Prefer summary labels, not sentence fragments or paragraph text
- Exception: when mirroring a standardized external field label or address-like business data contract, authors may stretch toward ~64 characters, but longer explanation belongs in `instructions`
- Examples: `Oven Temperature Test`, `Oven Display Check`

#### `action`
Short verb phrase describing the operator-facing task.

- Used for execution UIs and automation prompts
- Should be concise; default soft limit: ~32 characters
- Prefer short execution cues, not multi-clause procedure text
- Exception: when mirroring a standardized external field label or address-like business data contract, authors may stretch toward ~64 characters, but step-by-step detail belongs in `instructions`
- Examples: `Verify Temperature Stability`, `Verify Display Reading`

#### `spec`
Expected target or standard.

- Should be concise; default soft limit: ~32 characters
- Prefer short expected outcomes or field rules, not full validation prose
- Exception: when preserving standardized external field limits or address-like business data contracts, authors may stretch toward ~64 characters, but broader acceptance guidance belongs in `instructions`
- Examples: `180–190 °C`, `±2 °C`

#### `instructions`
Freeform SOP text that provides the details required to perform the action.

Guidelines:
- Long form allowed
- Recommended limit: 4096 characters
- If more detail is needed, include an external link
- Use to explain procedures without bloating `procedure`, `action`, or `spec`

**Identity rule:** All template fields above are immutable with respect to identity. Any change—including wording changes in `instructions`—produces a new `slug_id`.

---

### 3.2 Identity Fields and Addressing

Identity is split into template identity and instance identity.

#### `slug_id` (template identity)
A 16-character Crockford Base32 identifier for a template row. Derived deterministically from:

- `checklist`
- `section`
- `procedure`
- `action`
- `spec`
- `instructions`

Two rows with identical fields must yield the same `slug_id`. Any change to any field produces a new `slug_id`.

#### `instance_id`
A 16-character Crockford Base32 identifier derived from the instance principal string (Section 2.2) that identifies which copy of the template is in use.

#### Address tuple
The ordered pair:

(slug_id, instance_id)

This is the canonical runtime identity for a slug. All state, history, and evaluation attach to this tuple.

#### `address_id` (optional)
A reversible 32-character composite token:

- First 16 characters: `slug_id`
- Last 16 characters: `instance_id`

Formally:

address_id = slug_id || instance_id

`address_id` is a convenience for:
- APIs
- CLIs
- Logs
- Labels
- Training material (“hello world”)

Internally, the canonical key remains the address tuple `(slug_id, instance_id)`.

---

### 3.3 State Fields (Mutable Per Address)

State fields store the outcome of performing the action for a specific `(slug_id, instance_id)`.

#### `result`
Observed outcome.

- Soft UI limit: ~32 characters
- Examples: `21 °C`, `OK`, `24 V`

#### `status`
Enumerated outcome:

- `Pass`
- `Fail`
- `NA`
- `Other`

#### `comment`
Clarifying operator remarks.

- Recommended upper limit: 512 characters
- Should remain concise; longer narratives belong in external documents or attachments

#### `timestamp`
ISO 8601 UTC timestamp of the last update for this address. Runtime-managed; clients never set this field directly.

#### `entity_id`
16-character Crockford Base32 identifier of the actor that performed the update.

- Derived from runtime auth or environment
- Not authored in Markdown
- Not provided by minimal update calls

Section 7 defines how runtimes derive `entity_id` (including local-development defaults).

#### State field rules

- State fields may change arbitrarily; they do not affect `slug_id` or `instance_id`.
- `timestamp` and `entity_id` are regenerated by the runtime for every successful update.

##### Logging note (optional extension)

The core spec does not require recording every historical state change. Deployments that need logs may:

- Write client-side logs using the slug returned from each API update, or
- Maintain a separate logging database, or
- Add extension tables outside the core runtime schema.

The canonical runtime store is optimized for current state, not time-series telemetry.

---

### 3.4 Relationships (Template and Address Graphs)

Relationships are directed triples stored at two layers:

- **Address-level (runtime, primary):** `(subject_address_id, predicate_token, target_address_id)`
- **Template-level (authoring surface):** `(subject_slug_id, predicate_token, target_slug_id)`

`predicate_token` is stored under the field name `predicate` in the reference storage schema and APIs. The server treats it as opaque and does not attach semantics; semantics are applied by clients/daemons operating against the API (Section 9.5).

Key principles:

- Address-level relationships are first-class and intended for day-to-day operational wiring, including cross-instance edges.
- Template-level relationships are authored by checklist designers and typically change slowly.
- Template relationships are often projected into instance context by combining `slug_id` with a chosen `instance_id` (same-instance projection).

Example: if the template declares a relationship

SLUG_A <predicate_token> SLUG_B

then, for instance `INST_X`, tools can project that to address space as:

(SLUG_A || INST_X) <predicate_token> (SLUG_B || INST_X)

#### 3.4.1 Address-Level Relationships

Address-level relationships are first-class and are intended for day-to-day operational wiring, expressed directly in `address_id` space:

- `(subject_address_id, predicate_token, target_address_id)`

Semantics:

- `subject_address_id` identifies one specific slug at one specific instance.
- `target_address_id` identifies another specific slug at another specific instance (which may be the same instance or a different one).
- `predicate_token` is opaque at the server layer; the reference daemons MAY parse canonical tokens and apply deterministic behavior (Section 9.5).

Deployments MAY restrict permissions for template-level relationships while enabling broad use of address-level relationships. Proven, high-value address-level relationships may be promoted back into templates by authors.

**Storage**

Implementations SHOULD keep address-level relationships in a separate structure from template relationships, for example:

- `template_relationships(subject_slug_id, predicate, target_slug_id)`
- `address_relationships(subject_address_id, predicate, target_address_id)`

The two tables are intentionally separate layers: template relationships are portable; address relationships are deployment-local and operational.

**API Shape (Outline)**

Section 7 defines concrete API endpoints. A typical separation is:

- Template relationships:
  - `GET /api/v1/relationships/template?subject_slug_id=…`
  - `POST /api/v1/relationships/template` with `{ "subject_slug_id", "predicate", "target_slug_id" }`
- Address-level relationships:
  - `GET /api/v1/relationships/address?subject_address_id=…`
  - `POST /api/v1/relationships/address` with `{ "subject_address_id", "predicate", "target_address_id" }`

Servers MAY also provide combined views, for example returning:

- `template_relationships`: list of `{ predicate, target_slug_id }`
- `address_relationships`: list of `{ predicate, target_address_id }`
  - `has_address_relationships`: boolean hint

for a given `slug_id` or `address_id`, so UIs and agents can choose whether to auto-query address-level details.

  #### 3.4.2 Slug Lineage Relationships (Template + Address)

Slug lineage captures version chains when an edited row produces a new `slug_id`. Lineage is expressed using the existing relationship graph (no new tables). The canonical authoring surface is template-level:

  - `slugSuccessor` — `OLD_SLUG -> NEW_SLUG`
  - `slugPredecessor` — `NEW_SLUG -> OLD_SLUG` (reverse edge for convenience)

Address-level variants MAY be used for instance overlays:

  - `addressSuccessor` — `OLD_ADDR -> NEW_ADDR`
  - `addressPredecessor` — `NEW_ADDR -> OLD_ADDR`

Guidance:
  - Lineage SHOULD remain associated with a single slug.
  - Chains SHOULD be linear; tools SHOULD warn on cycles or multiple successors.
  - Lineage does not change slug hashing or identity rules; it is additive metadata.
  - "Latest slug" resolution is a traversal of `slugSuccessor` edges; ambiguity yields warnings.

**Markdown Authoring Guidance (Optional, Deployment-Local)**

Template relationships remain the canonical authoring surface and are expressed under `#### Relationships` using a source identity line plus relationship bullets (Section 5.7), for example:

- `SLUG_A`
- `<predicate_token> SLUG_B`

Authoring tools MAY also accept the same-checklist shorthand `(section, procedure)` as described in Section 5.7. If the source identity line uses address identity, the relationships are treated as address-level and stored in `address_relationships` instead.

Address-level relationships MAY be encoded in Markdown as a **deployment-local overlay**, for example by adding lines prefixed with `@address`:

- `@address AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHH passSyncValidatedPass IIIIJJJJKKKKLLLLMMMMNNNNOOOOPPPP`

Rules:

- These `@address` lines are OPTIONAL and deployment-specific.
- Importers that do not support address-level relationships MUST ignore `@address` lines safely.
- Exporters that include address-level relationships SHOULD clearly label them as deployment-local, and MUST NOT require them for core template semantics.

In other words, template-level relationships are portable and shared; address-level relationships are a deployment-local operational layer.

---

### 3.5 Canonical Slug Shape (Conceptual Object)

At runtime, a fully realized slug for a specific instance has the following conceptual shape:

- Template fields:
  - `checklist`
  - `section`
  - `procedure`
  - `action`
  - `spec`
  - `instructions`

- Identity:
  - `slug_id`
  - `instance_id`
  - `address_id` (optional, derived)

- Ordering:
  - `address_order` (derived, per instance)

- State:
  - `result`
  - `status`
  - `comment`
  - `timestamp`
  - `entity_id`

- Relationships:
  - `template_relationships`: list of objects shaped as `{ predicate, target_slug_id }`
  - `address_relationships`: list of objects shaped as `{ predicate, target_address_id }`

Sections 5–7 define how this conceptual structure is realized in:

- Markdown authoring
- SQLite storage schema
- JSON/JSONL payloads
- HTTP API and MCP interfaces

### Echo Slug (Optional Runtime Feature)

Implementations MAY provide an “echo slug” response for client-side logging and audit purposes.

If enabled, a successful state update MAY return the fully resolved slug for the updated address, including:

- all template fields
- `slug_id`
- `instance_id`
- `address_id` (if applicable)
- `address_order` (if applicable)
- current state fields (`result`, `status`, `comment`, `timestamp`)
- `entity_id`

Rules:

- Echo Slug MUST NOT alter state; it is a read-only serialization.
- Echo Slug MUST use the same canonical encoding and normalization rules defined in Section 4.
- Echo Slug MUST include `entity_id`, since this represents the authenticated or fallback actor responsible for the update.
- Echo Slug MUST be opt-in at the API level (e.g., via a request flag), so deployments that hide `entity_id` from operators may disable it.
- Echo Slug responses MUST be treated by clients as immutable log records.

This mechanism avoids embedding a historical log inside the runtime database while providing a consistent, spec-defined method for external logging systems (CLI tools, UI clients, automations) to capture complete state transitions.

## 4. Canonical Encoding and Normalization

This section defines the **canonicalization rules** that apply before computing IDs or storing identity-bearing fields. The goal is:

- identical human content → identical `slug_id`, `instance_id`, `entity_id` across deployments
- strict, predictable behavior for tools and LLMs
- explicit limits to avoid accidental bloat

These rules are **normative**. Any implementation that diverges from them may compute different IDs for the same authoring content.

### 4.1 Identity vs. Non-Identity Fields

**Identity-bearing fields** (inputs to IDs):

- Template identity (for `slug_id`):
  - `checklist`
  - `section`
  - `procedure`
  - `action`
  - `spec`
  - `instructions`
- Instance identity (for `instance_id`):
  - instance principal string
- Entity identity (for `entity_id`):
  - entity principal string
- Relationship predicates:
  - `predicate` (strict token)

**Non-identity fields** (do *not* affect any ID):

- State:
  - `result`
  - `status`
  - `comment`
  - `timestamp`
  - `entity_id` (value is an ID, not an input to another ID)
- Relationships:
  - `target_slug_id` (references, not inputs)
- Runtime/storage metadata:
  - history rows
  - internal numeric IDs (`checklist_id`, `section_id`, etc.)

### 4.2 Global Encoding Rules (All Text Fields)

All text fields in the spec **MUST** obey:

- Encoding:
  - MUST be UTF-8 on the wire and in storage.
- Unicode normalization:
  - MUST be normalized to Unicode NFC before any comparison or hashing.
- Line endings:
  - MUST normalize any `CRLF` (0x0D 0x0A) or `CR` (0x0D) to `LF` (0x0A) on ingest.
- Control characters:
  - MUST NOT contain ASCII control characters in the range 0x00–0x08, 0x0B, 0x0C, 0x0E–0x1F.
  - `LF` (0x0A) is allowed only where explicitly permitted (e.g., in `instructions`).
- Tabs:
  - Input TAB (0x09) is **not allowed** in identity-bearing fields (see 4.3); in other fields, deployments MAY normalize TAB → SPACE or reject input. The reference implementation treats TAB as invalid in all fields.

Any violation SHOULD result in a validation error at ingest time.

### 4.3 Canonicalization for Template Identity Fields

This applies to:

- `checklist`
- `section`
- `procedure`
- `action`
- `spec`
- `instructions` (with additional rules for multiline)

#### 4.3.1 `checklist`

- Purpose: stable identifier for the checklist.
- Canonicalization:

  - MUST be UTF-8, NFC.
  - MUST NOT contain `LF` (single-line).
  - MUST NOT contain TAB.
  - MUST trim leading and trailing Unicode whitespace.
  - MUST NOT be empty after trimming.
  - **Size**:
    - MUST NOT exceed 128 Unicode scalar values.
  - Origin:
    - MAY be derived from filename (without extension), or
    - MAY be taken from front-matter `Filename:` (see 3.1 examples).
    - If both exist, deployments MUST define a deterministic precedence rule; the reference implementation uses front-matter if present, otherwise filename.

The canonical string used for hashing is the **post-normalization** value.

#### 4.3.2 `section`

- Purpose: structural grouping; part of template identity.
- Canonicalization:

  - MUST be UTF-8, NFC.
  - MUST NOT contain `LF` (single-line).
  - MUST NOT contain TAB.
  - MUST trim leading and trailing whitespace.
  - MAY be empty (for checklists without sections).
  - **Size**:
    - MUST NOT exceed 64 Unicode scalar values.

Any change to `section` creates a new `slug_id`.

#### 4.3.3 `procedure`

- Purpose: noun phrase for the work item.
- Canonicalization:

  - MUST be UTF-8, NFC.
  - MUST NOT contain `LF` (single-line).
  - MUST NOT contain TAB.
  - MUST trim leading and trailing whitespace.
  - MUST NOT be empty after trimming.
  - **Size**:
    - MUST NOT exceed 32 Unicode scalar values.

Examples (non-normative):

- "Oven Temperature Test"
- "Oven Display Test"

#### 4.3.4 `action`

- Purpose: verb phrase for the operator-facing task.
- Canonicalization:

  - MUST be UTF-8, NFC.
  - MUST NOT contain `LF` (single-line).
  - MUST NOT contain TAB.
  - MUST trim leading and trailing whitespace.
  - MUST NOT be empty after trimming.
  - **Size**:
    - MUST NOT exceed 32 Unicode scalar values.

Examples (non-normative):

- "Verify temperature stability"
- "Verify temperature display"

#### 4.3.5 `spec`

- Purpose: concise expected target or standard for evaluation.
- Canonicalization:

  - MUST be UTF-8, NFC.
  - MUST NOT contain `LF` (single-line).
  - MUST NOT contain TAB.
  - MUST trim leading and trailing whitespace.
  - MAY be empty (for procedures without a numeric/explicit spec).
  - **Size**:
    - MUST NOT exceed 32 Unicode scalar values.

Examples (non-normative):

- "180–190 °C"
- "5 V ± 0.1 V"

Specifications like "within tolerance" SHOULD be avoided in favor of measurable statements.

#### 4.3.6 `instructions`

- Purpose: SOP text; part of template identity so that instruction changes produce new `slug_id`.
- Canonicalization:

  - MUST be UTF-8, NFC.
  - MUST normalize all line endings to `LF` (0x0A).
  - MUST NOT contain TAB.
  - Leading and trailing whitespace:
    - MUST trim leading and trailing lines that are entirely whitespace.
    - MUST trim leading and trailing whitespace *within* those boundary lines.
    - Internal lines MAY retain leading/trailing spaces (for formatting).
  - **Size**:
    - MUST NOT exceed 4096 Unicode scalar values.

- Newlines:

  - `instructions` MAY contain internal `LF`s.
  - For identity, the canonical string is the entire normalized multi-line text, including internal `LF`s.

Deployments SHOULD provide tools to help authors stay well below 4096 characters by linking out to external documents when longer material is needed. Authoring clients SHOULD warn or block on overflow; the server MUST enforce the limit.

### 4.4 Canonicalization for Instance Principals (`instance_id`)

The **instance principal string** is the input to `instance_id`. It is defined by the deployment and is treated as opaque text by the core.

Canonicalization pipeline for the principal string:

- MUST be UTF-8, NFC.
- MUST normalize line endings to `LF`.
- MUST NOT contain `LF` (instance principals are single-line).
- MUST NOT contain TAB.
- MUST trim leading and trailing whitespace.
- MUST NOT be empty after trimming.
- MAY use the literal delimiter string " || " internally, but deployments SHOULD treat that as part of their own formatting conventions.

**Size**:

- MUST NOT exceed 256 Unicode scalar values.

Example shapes (non-normative):

- "machine || model=Machine_A || serial=1234"
- "line || plant=CT || line_id=Furnace_2"
- "room || building=Lab || room=201"

After canonicalization, the principal string is:

1. fed into the hash function (see 4.6), and
2. surfaced to authoring-capable clients via the instance catalog or an equivalent adapter even if stored outside `slugs`; redaction follows deployment flags (Section 13).

Deployments SHOULD seed a canonical root/template principal (e.g., `template||default`) and return it from list operations when callers omit `instance_id` (see Sections 7.4.4 and 10.2.4).

### 4.5 Canonicalization for Entity Principals (`entity_id`)

The **entity principal string** is the input to `entity_id`. It is also deployment-defined and opaque to the core.

Canonicalization pipeline:

- MUST be UTF-8, NFC.
- MUST normalize line endings to `LF`.
- MUST NOT contain `LF` (single-line).
- MUST NOT contain TAB.
- MUST trim leading and trailing whitespace.
- MUST NOT be empty after trimming.

**Size**:

- MUST NOT exceed 256 Unicode scalar values.

Example shapes (non-normative):

- "idp:azuread || 00000000-0000-0000-0000-000000000000"
- "system || checklist-ingest-worker-01"
- "agent || checklist-assistant-llm-v1"

After canonicalization, the principal string is used as the hash input for `entity_id`.

### 4.6 Canonical Address Strings for Hashing

This section fixes the canonical byte strings used to compute IDs.

#### 4.6.1 Slug ID (`slug_id`)

The `slug_id` is computed from a **canonical addressing string** built from the canonicalized template fields:

- `checklist`
- `section`
- `procedure`
- `action`
- `spec`
- `instructions`

Construction rules:

1. Canonicalize each field as per 4.3.
2. Join them with the literal delimiter:

" || "

That is, the exact string: space, vertical bar, vertical bar, space.
3. The resulting Unicode string is converted to UTF-8 bytes.
4. Compute the xxHash128 of those bytes.
5. Extract the lowest 80 bits (10 bytes) of the hash.
6. Encode the 10 bytes in Crockford Base32 (unambiguous alphabet, uppercase), yielding a 16-character `slug_id`.

Constraints:

- None of the fields may contain the literal " || " sequence; ingest must reject such input for identity-bearing fields.
- Any change in any template field after canonicalization (including minor edits to `instructions`) leads to a different `slug_id`.

#### 4.6.2 Instance ID (`instance_id`)

The `instance_id` is computed from the **canonical instance principal string** after processing in 4.4:

1. Take the canonical principal string (single-line, NFC, trimmed).
2. Convert to UTF-8 bytes.
3. Compute xxHash128.
4. Extract lowest 80 bits (10 bytes).
5. Encode in Crockford Base32 (uppercase), yielding a 16-character `instance_id`.

The mapping principal → `instance_id` MUST be deterministic within a deployment.

#### 4.6.3 Entity ID (`entity_id`)

The `entity_id` is computed analogously, from the canonical entity principal string (4.5):

1. Canonical principal string → UTF-8 bytes.
2. xxHash128.
3. Lowest 80 bits.
4. Crockford Base32 (uppercase) → 16-character `entity_id`.

Deployments MAY choose a different hash function, but MUST preserve:

- deterministic mapping,
- 80-bit entropy,
- 16-character Crockford Base32, uppercase encoding.

### 4.7 Non-Identity Field Rules (Result, Status, Comment)

Although non-identity fields do not affect IDs, they still have strict constraints.

#### 4.7.1 `result`

- Purpose: concise observed outcome for comparison with `spec`.
- Rules:

  - MUST be UTF-8, NFC.
  - MUST NOT contain `LF` (single-line).
  - MUST NOT contain TAB.
  - MUST trim leading and trailing whitespace.
  - MAY be empty.
  - **Size**:
    - MUST NOT exceed 32 Unicode scalar values.

Examples (non-normative):

- "182 °C"
- "4.98 V"
- "OK"

#### 4.7.2 `status`

- Purpose: enumerated evaluation outcome.
- Rules:

  - MUST be one of:
    - "Pass"
    - "Fail"
    - "NA"
    - "Other"
  - MUST be case-sensitive as above.
  - Any other value MUST be rejected at ingest.

#### 4.7.3 `comment`

- Purpose: operator or system commentary.
- Rules:

  - MUST be UTF-8, NFC.
  - MUST NOT contain `LF` (single-line).
  - MUST NOT contain TAB.
  - MUST trim leading and trailing whitespace.
  - MAY be empty.
  - **Size**:
    - MUST NOT exceed 512 Unicode scalar values.

Deployments needing multi-line commentary SHOULD store it in external systems or derived views, not in `comment`. Such a system should encourage the comment to reference this alternative verbose storage (ex. File path or URL to a photo can make for a good comment)

### 4.8 Newline Normalization Summary

For clarity:

- Fields that MUST be **single-line** (no `LF`):

  - `checklist`
  - `section`
  - `procedure`
  - `action`
  - `spec`
  - instance principal string
  - entity principal string
  - `result`
  - `comment`

- Fields that MAY be **multi-line** (with normalized `LF`):

  - `instructions`

All inputs MUST normalize any CRLF/CR sequences to LF before validation, and any remaining `LF` MUST obey the above rules.

### 4.9 Invariants

Given the rules above:

- Any two deployments that:
  - ingest the same Markdown template,
  - derive the same instance and entity principal strings,

will compute identical:

  - `slug_id` for each template row,
  - `instance_id` for each instance principal,
  - `entity_id` for each entity principal,
  - `address_id` for each `(slug_id, instance_id)` pair.

- Identity is **stable** under:

  - storage round-trips,
  - JSON/JSONL exports,
  - SQLite migrations,

as long as canonicalization is applied consistently on ingest.

- Deviating from these canonicalization rules (e.g., preserving CRLF, allowing TABs, skipping NFC) is considered **non-conformant** and may result in divergent IDs for otherwise identical content.

## 5. Markdown Authoring Specification (Canonical at Authoring Time)

Markdown is the canonical human authoring format. One Markdown file defines one **Checklist Template**. The runtime store (SQLite) is canonical for state; Markdown is canonical for structure and intent.

This section defines the **only** Markdown layout that tools MUST accept as canonical.

---

### 5.1 File-Level Structure

A checklist file consists of:

1. Optional metadata front-matter block.
2. One required checklist heading (level 1).
3. Zero or more sections (level 2).
4. Under each section, zero or more **procedure blocks** (level 3).
5. Fixed bullet fields and subsections under each procedure block.

The overall pattern is:

- optional front-matter metadata
- `# Checklist: <checklist>`
- `## Section: <section>`
- `### Procedure: <procedure>`
  - bullets for `Action`, `Spec`, `Result`, `Status`, `Comment`
  - `#### Instructions`
  - `#### Relationships`

#### Compatibility Variants (Optional)

Implementations MAY accept the following compatibility variants and normalize them to the canonical form before hashing or storage:

- Plain headings without prefixes: `# <checklist>`, `## <section>`, `### <procedure>`.
- Bullet labels with emphasis: `- **Action:** ...` or `- __Action__: ...`.
- Front-matter list items: `- Checklist: ...` and other metadata keys.
- Separator lines (`---`) between procedure blocks (outside front-matter) MAY appear and MUST be ignored.
- UTF-8 BOMs at the start of files MUST be stripped before parsing.

Importers MAY apply additional normalization or sanitization (for example, pruning unsupported Markdown constructs) as long as the resulting template fields obey the canonicalization rules in Section 4.

---

### 5.2 Optional Front-Matter Metadata

A file MAY start with a simple metadata block:

    ---
    Filename: checklist template
    Version: 0.0.1-0.5.6
    Date: 2025-11-29T15:03:00Z
    Organization: Example Organization
    Project: Checklist Assistant
    ---

Rules:

- The delimiter lines MUST be exactly three hyphens (`---`) on their own line.
- Keys MUST be simple ASCII words with a colon separator.
- Keys MAY be written as list items with a leading `- ` (for example `- Checklist: ...`).
- Front-matter is **not** used in `slug_id` computation.
- If a `Checklist:` key is present here, it MUST match the `<checklist>` text in the level-1 heading exactly. For list-item keys, ingest MAY treat mismatches as warnings while still honoring the heading text.

Front-matter is purely informational and for versioning; ingest tools MAY ignore it except for validation and display.

---

### 5.3 Checklist Heading (Level 1)

Immediately after optional front-matter, the file MUST contain exactly one checklist heading:

    # Checklist: <checklist>

Rules:

- The literal prefix `Checklist:` MUST appear exactly (case-sensitive, with trailing colon and space).
- `<checklist>` is the checklist name used as the `checklist` template field.
- The `<checklist>` text MUST respect the constraints from Section 3 (UTF-8 NFC, character and length rules, Windows filename-safe if used as filenames).

No other level-1 headings are allowed in the file.

---

### 5.4 Section Headings (Level 2)

Each section is introduced by a level-2 heading:

    ## Section: <section>

Rules:

- The literal prefix `Section:` MUST appear exactly.
- `<section>` becomes the `section` template field for all procedure blocks that follow, until the next `## Section:` heading or end of file.
- Sections MAY appear in any order.
- Sections MUST NOT be nested (no section under another section).

A file MAY have zero or many sections. If no `## Section:` appears, ingestion MUST treat this as an error (every procedure requires a section).

---

### 5.5 Procedure Blocks (Level 3)

Each checklist row (template-level) is represented by a **procedure block**:

    ### Procedure: <procedure>
    - Action: <action>
    - Spec: <spec>
    - Result: <result or empty>
    - Status: <Pass|Fail|NA|Other or empty>
    - Comment: <comment or empty>

    #### Instructions
<freeform markdown text>

    #### Relationships
    - <slug_id>
    - <predicate> <target_slug_id>

Rules:

1. Heading:
   - The level-3 heading MUST start with `### Procedure:`.
   - `<procedure>` becomes the `procedure` template field.
   - `<procedure>` MUST obey the length and encoding limits from Section 4 (hard limit, UTF-8 NFC, no newlines).

2. Bullet field block:
   - Exactly five bullets MUST appear immediately after the procedure heading, in this fixed order:

        - Action: ...
        - Spec: ...
        - Result: ...
        - Status: ...
        - Comment: ...

   - Bullet markers MUST be a single hyphen + single space (`- `).
   - Field name capitalization and spelling MUST be exactly: `Action`, `Spec`, `Result`, `Status`, `Comment`.
   - For compatibility, tools MAY accept field names wrapped in `**` or `__` (for example `- **Action:** ...`) and normalize them to the canonical labels.
   - A colon and single space (`: `) MUST follow the field name.
   - The remainder of the line is the field value; leading/trailing whitespace after the colon is trimmed.
   - `Action` and `Spec` MUST be non-empty and satisfy their length/encoding constraints.
   - `Result`, `Status`, `Comment` MAY be empty at authoring time.
   - If `Status` is non-empty, its value MUST be one of `Pass`, `Fail`, `NA`, `Other`.

3. Ordering:
   - No other content may appear between `### Procedure: <procedure>` and the first `- Action:` bullet.
   - No blank lines are required between bullets, but blank lines are allowed.

4. Subsections:
   - Only `#### Instructions` and `#### Relationships` are allowed as level-4 headings.
   - `#### Instructions` MUST appear exactly once and MUST precede `#### Relationships`.
   - `#### Relationships` MAY be omitted if there are no relationships to declare.

#### Checklist metadata slug (first procedure block)

- Deployments MUST seed a metadata slug as the first procedure block when creating a new checklist (for example via UI, CLI, or importer). Authors MAY edit it like any other row; ingest MAY inject it if absent to enforce the invariant.
- Shape: it uses the same procedure-block structure (section, procedure, Action/Spec/Result/Status/Comment bullets, Instructions). Recommended defaults:
  - Section: `_meta` (or deployment-chosen reserved section)
  - Procedure: `Checklist metadata`
  - Action/Spec: short description of the checklist purpose/scope
  - Result/Status/Comment: empty or `NA` by default (clients MAY hide NA rows in execution UIs/reports)
  - Instructions: freeform metadata about the checklist (authorship, scope, safety notes, links)
- Clients MAY choose to hide or style this row specially, but parsers MUST treat it as a normal slug with a deterministic `slug_id`.
- Keeping the metadata slug as an ordinary row avoids bespoke “header” formatting while preserving a place to store checklist-level context alongside rows.

Multiple `### Procedure:` blocks MAY appear under a single `## Section:`. Each procedure block expands into one template row and one `slug_id`.

---

### 5.6 Instructions Subsection (Level 4)

Every procedure block MUST include exactly one `Instructions` subsection:

    #### Instructions
<freeform markdown text, zero or more lines>

Rules:

- The heading MUST be exactly `#### Instructions` (no colon).
- All text from the first line after this heading up to (but not including) the next `####` or `###` or `##` or end-of-file belongs to the `instructions` field.
- Headings at levels 5 or 6 (`#####`/`######`) are allowed inside `instructions` and do not terminate the block.
- Deeper heading structures SHOULD link out to external SOPs rather than nesting further.
- Any valid Markdown is allowed in the instructions body (links, lists, inline code, etc.), subject to the size and encoding limits from Section 4.
- Authors SHOULD place detailed step-by-step SOP content, safety notes, and references here.
- For longer content, authors SHOULD link to external documents rather than embedding full procedures.

Instructions content participates in `slug_id` identity: changing instructions changes `slug_id`. Canonicalization trims leading/trailing blank lines from `instructions` before hashing (Section 4.3.6).

---

### 5.7 Relationships Subsection (Level 4)

Every procedure block MAY include a `Relationships` subsection:

    #### Relationships
    - <slug_id>
    - <predicate> <target_slug_id>

Rules:

- The heading MUST be exactly `#### Relationships`.
- The first bullet line under this heading MUST provide the source identity and does not describe an outgoing relationship.
- Bullet markers MUST be `- `.
- The source identity line MUST be one of:
  - `<slug_id>` (16-character Crockford Base32), or
  - `<address_id>` (32-character Crockford Base32).
- If the source identity line is an address identity (`address_id`), the relationships are address-level; otherwise they are template-level.
- For initial authoring, the source identity line MAY be omitted. In that case, ingest SHOULD assume the current procedure’s computed `slug_id` as the source and treat the first bullet as a relationship line (template-level only).
- Each remaining bullet line under this heading (until the next `####` or `###` or `##` or end-of-file) describes one outgoing relationship.
- Each relationship bullet MUST have the form:

<predicate> <target>

where `<target>` is one of:
  - `<target_slug_id>` (16-character Crockford Base32 `slug_id`), or
  - `<target_address_id>` (32-character Base32 `address_id`), or
  - `(section, procedure)` for same-checklist shorthand.

- `<predicate>` is a predicate token matching the lexical rules in Section 11.9 (canonical tokens per Section 9.5.2 are recommended when the reference daemons should act on the relationship).
- A single space MUST separate `<predicate>` and `<target>`.
- No additional trailing text is allowed on the line (no inline comments).
- `(section, procedure)` is a same-checklist shorthand intended for pre-hash authoring.
- Cross-instance references MUST use `address_id`.
- Cross-checklist references SHOULD use `slug_id` or `address_id`.
- If `(section, procedure)` is not unique, authors SHOULD use `slug_id`. The deployment must Warn or Err for ambiguous conditions
- If a tuple target matches multiple rows, ingestion MUST emit an ambiguity warning or error. Deployments MAY attach the relationship to all matches or reject the relationship to avoid unintended writes.

Notes:

- Relationships are authored in terms of template identities (`slug_id`). Practical editing workflows can:
  - generate `slug_id`s on an initial ingest, and
  - help authors insert or autocomplete `target_slug_id` values.
- Because the source identity line is ID-based, tool-assisted authoring SHOULD backfill it after the first ingest.
- The `(section, procedure)` shorthand improves manual readability, allows authoring before hash IDs are known, and provides a shorthand for common same-checklist references.
- Exporters SHOULD emit `predicate <slug_id>` targets by default and MAY emit `(section, procedure)` only for same-checklist targets when unambiguous.
- Permissioned clients MAY export both outgoing and incoming relationships for a row to support migration/rename workflows and relationship-aware tooling. This is not automated in the reference implementation and is provided as an example of possible client behavior. One optional layout is:

    #### Relationships
    - <source_slug_id>

    ##### Outgoing
    - <predicate> <target_slug_id>

    ##### Incoming
    - <source_slug_id> <predicate>

Importers MAY ignore the `##### Outgoing`/`##### Incoming` headings and treat the section as read-only metadata; deployments SHOULD document whether these blocks are intended for ingest. Large, multi-checklist deployments SHOULD assume tool-assisted authoring to keep relationships consistent as addressable fields evolve.
- Expanded tuple or JSONL forms are deployment-local authoring aids and are out of scope for the reference implementation; if accepted, they MUST be normalized to `slug_id`/`address_id` before ingest.
- Ingestion MUST treat any malformed relationship line as a warning and ignore that line, without aborting the entire file.

If a procedure has no relationships, the whole `#### Relationships` section MAY be omitted. If present without outgoing relationships, it MAY contain only the source identity line.

---

### 5.8 State Bullets at Authoring Time

The bullets:

- `Result:`
- `Status:`
- `Comment:`

are present in the canonical Markdown layout but are **not** authoritative at runtime.

Rules:

- Ingest tools MAY use initial `Result`, `Status`, `Comment` values as a seed for initial state on first import.
- Once the slug exists in the runtime store, all subsequent state changes MUST flow through the API/MCP; Markdown values are ignored on re-ingest except for compatibility or migration tools.
- Deployments that wish to treat Markdown state as advisory MUST do so explicitly and document that behavior separately from this spec.

---

### 5.9 Parsing Rules (Normative)

Given a Markdown file, ingestion MUST:

1. Strip any UTF-8 BOM, normalize line endings to `\n`, and normalize text to UTF-8 NFC before parsing.
2. Optionally parse and validate front-matter, then discard it for identity.
3. Ignore standalone `---` separator lines outside front-matter.
4. Locate the single `# Checklist:` heading; extract `<checklist>` as the `checklist` template field.
5. For each `## Section:` heading in order:
   - Set the current `section` template field to `<section>`.
6. For each `### Procedure:` heading under the current section:
   - Create a new template row.
   - Read the next five bullet lines in order (`Action`, `Spec`, `Result`, `Status`, `Comment`), enforcing field names and formats.
   - Attach the immediately following `#### Instructions` block as `instructions`.
   - Attach any `#### Relationships` block by:
    - Parsing the first bullet as the source identity line (ignore for relationship creation).
    - Parsing each remaining bullet as a relationship using the accepted target forms (`slug_id`, `address_id`, `(section, procedure)`).
    - Resolving `(section, procedure)` to slug_id matches within the same checklist; if multiple rows match, attach relationships to all matching rows and emit an ambiguity warning.
   - Compute `slug_id` from the template fields as defined in Section 4.

Validation:

- Missing or misordered bullets MUST cause ingestion to fail for that procedure block (and SHOULD be reported clearly).
- Missing `#### Instructions` MUST cause ingestion to fail for that procedure block.
- Unknown or malformed `Status` values MUST be rejected.
- Violations of length/encoding constraints MUST be rejected.

A file with any invalid procedure blocks MAY still ingest valid blocks, but deployments SHOULD log and surface such partial-ingest conditions to authors.

---

### 5.10 Minimal Example

A minimal well-formed checklist file:

    # Checklist: Oven Temperature Checks

    ## Section: Temperature Stability

    ### Procedure: Oven Temperature Test
    - Action: Verify oven temperature stability
    - Spec: 180–190 °C
    - Result: 
    - Status: 
    - Comment: 

    #### Instructions
Preheat the oven to setpoint and monitor for at least 10 minutes. Record the stabilized reading from the reference thermometer.

    #### Relationships
    - BCDFGHJKMNPQRSTV
    - passPropagateValidatedPass ABCDEFGHJKMNPQRS

    ## Section: Display Verification

    ### Procedure: Oven Display Test
    - Action: Verify oven temperature display
    - Spec: Display within ±2 °C of reference
    - Result: 
    - Status: 
    - Comment: 

    #### Instructions
Compare the oven's built-in display with the reference thermometer at the stabilized temperature.

    #### Relationships
    - JKMNPQRSTVWXYZ23
    - passPropagateValidatedPass ABCDEFGHJKMNPQRS

Each `### Procedure:` block above becomes one `slug_id`. Ingest tooling MUST auto-seed the metadata slug described in Section 5.5 if it is absent; it is omitted here for brevity in this example. At runtime, combining each `slug_id` with a chosen `instance_id` yields the address tuple `(slug_id, instance_id)` and (optionally) the composite `address_id`.

Note: In the typical workflow, each `### Procedure:` block becomes one `slug_id`. This is usually sufficient for authoring and review. However, as defined in Section 2.2, `slug_id` depends on ALL immutable template fields (`checklist`, `section`, `procedure`, `action`, `spec`, `instructions`). Any change to any of these fields creates a NEW `slug_id`. Authors should be aware that revising a checklist (renaming sections, editing procedures, or updating instructions) will create new slugs and may leave older slugs present in a deployment until they are explicitly removed or migrated.

## 6. SQLite Storage Specification (DB storage is Canonical at Runtime)

SQLite is the canonical runtime store for Checklist Assistant checklists. All slug data, relationships, entities, and state histories are persisted here. Markdown and the Entity Principal Template Checklist are transformed (via the API/MCP) into the canonical data model and stored in SQLite; all API operations read and write exclusively through this runtime database.

The schema is optimized for:

- deterministic identity (`slug_id`, `instance_id`, `entity_id`, `address_id`)
- compact storage
- fast lookup by ID
- efficient traversal of relationship graphs
- compatibility with future connectors and exports

All writes (including ingestion) MUST go through the API/MCP layer; no component is permitted to bypass the API and write directly to SQLite.

> Principle: The core server should be able to run on small hardware when needed, allowing localized deployments for sensitive procedures while preserving the same export format for broader reporting, review, or federation.

### 6.1 Overview of Tables

Core tables:

1. `slugs` — current state of every checklist slug, keyed by `address_id`
2. `slug_order` — per-instance ordering of addresses for export/UI rendering
3. `slug_ownership` — source/pack ownership for template rows keyed by `slug_id`
4. `address_ownership` — source/pack ownership for runtime rows keyed by `address_id`
5. `template_relationships` — directed template-level edges keyed by `slug_id`
6. `address_relationships` — address-level edges keyed by `address_id` (deployments may disable)
7. `history` — append-only snapshots of mutable state over time
8. `entities` — catalog of entities (humans, robots, agents) keyed by `entity_id`

Optional governance/metadata tables:

9. `predicates` — catalog of known predicate tokens (optional governance)
10. `instance_catalog` — catalog of instances keyed by `instance_id` (deployment-local)

Additional indexes support fast lookup and graph traversal.

### 6.2 Table: `slugs`

`slugs` stores the canonical representation of each checklist slug at runtime: template fields, identity, mutable state, and runtime metadata.

```sql
    CREATE TABLE slugs (
        address_id   TEXT PRIMARY KEY,  -- 32-char Base32 = slug_id || instance_id

        -- Template fields (immutable per slug_id)
        checklist    TEXT NOT NULL,
        section      TEXT NOT NULL,
        procedure    TEXT NOT NULL,
        action       TEXT NOT NULL,
        spec         TEXT NOT NULL,
        instructions TEXT NOT NULL,

        -- Identity decomposition (redundant but query-friendly)
        slug_id      TEXT NOT NULL,     -- 16-char Base32
        instance_id  TEXT NOT NULL,     -- 16-char Base32

        -- Mutable state
        result       TEXT,
        status       TEXT CHECK (status IN ('Pass','Fail','NA','Other')),
        comment      TEXT,

        -- Runtime metadata
        timestamp    TEXT,              -- ISO8601 UTC
        entity_id    TEXT NOT NULL,     -- 16-char Base32

        FOREIGN KEY (entity_id) REFERENCES entities(entity_id)
    );
```

Rules:

- `address_id` is the canonical primary key: `slug_id || instance_id`.
- Template fields (`checklist`, `section`, `procedure`, `action`, `spec`, `instructions`) MUST match canonicalized template content and MUST NOT be mutated in place. Any change to these fields produces a new `slug_id` and therefore a new `address_id`.
- `slug_id` and `instance_id` are stored explicitly for convenience and indexing; they MUST match the decomposition of `address_id`.
- `result`, `status`, `comment`, `timestamp`, `entity_id` are mutable and updated only via the minimal update contract (Section 10).
- `entity_id` MUST always reference a valid row in `entities`.

### 6.2.1 Table: `slug_order`

`slug_order` stores the per-instance ordering for each address. It is derived from import order and is not part of the authoring contract.

```sql
    CREATE TABLE slug_order (
        address_id    TEXT PRIMARY KEY,  -- 32-char Base32 = slug_id || instance_id
        address_order INTEGER NOT NULL,

        FOREIGN KEY (address_id) REFERENCES slugs(address_id)
    );
```

Rules:

- Ordering is per instance: the same `slug_id` MAY have different `address_order` values.
- Importers MUST assign `address_order` in top-to-bottom Markdown order using sparse integers.
- New rows default to the end of their section; the runtime MAY repack values when gaps are exhausted.
- Ordering affects presentation/export order only and does not change identity or relationships.

### 6.2.2 Tables: `slug_ownership` and `address_ownership`

Ownership tables store the source/pack context that is deliberately not part of `slug_id` or `address_id`. This allows two packs to reuse identical row identities when the row content is identical, while still allowing workspace import/export/report operations to select the correct checklist folder.

```sql
    CREATE TABLE slug_ownership (
        slug_id       TEXT NOT NULL,
        checklist     TEXT NOT NULL,
        source_name   TEXT NOT NULL,
        source_path   TEXT,
        pack          TEXT NOT NULL,
        checklist_dir TEXT NOT NULL,
        updated_at    TEXT NOT NULL,
        PRIMARY KEY (slug_id, source_name, pack, checklist_dir)
    );

    CREATE TABLE address_ownership (
        address_id    TEXT NOT NULL,
        slug_id       TEXT NOT NULL,
        instance_id   TEXT NOT NULL,
        checklist     TEXT NOT NULL,
        source_name   TEXT NOT NULL,
        source_path   TEXT,
        pack          TEXT NOT NULL,
        checklist_dir TEXT NOT NULL,
        updated_at    TEXT NOT NULL,
        FOREIGN KEY(address_id) REFERENCES slugs(address_id) ON DELETE CASCADE,
        PRIMARY KEY (address_id, source_name, pack, checklist_dir)
    );
```

Rules:

- `source_name` identifies the configured checklist source, such as `public` or a local extra root name.
- `pack` and `checklist_dir` identify the owning folder under `checklists/<pack>/<checklist>/`.
- The parsed checklist name remains in `checklist`; it MAY match `checklist_dir`, but filesystem ownership MUST use `checklist_dir`.
- The same `slug_id` or `address_id` MAY have multiple ownership rows when identical content is intentionally shared by multiple packs.
- Workspace import MUST upsert ownership rows for imported template addresses; creating instance rows from a template SHOULD inherit template address ownership.
- Workspace export/report/list operations SHOULD filter by ownership when `source_name`, `pack`, or `checklist_dir` is supplied or can be inferred from a single persisted owner.
- If a checklist instance has multiple persisted owners and the request does not specify enough source/pack context, the API SHOULD return an ambiguity error rather than guessing.

### 6.3 Relationship Tables

Checklist Assistant stores relationships at two distinct layers:

- **Template-level** — between template identities (`slug_id`)
- **Address-level** — between runtime addresses (`address_id`)

Template-level and address-level relationships are both first-class within the specification. Deployments MAY disable address-level relationships (storage and endpoints) if they do not need cross-instance or operational wiring, but the reference implementation treats both layers as supported.

#### 6.3.1 Table: `template_relationships`

Template relationships encode relationships between template rows, independent of instances. They implement the triples described in Section 3.4.

```sql
    CREATE TABLE template_relationships (
        subject_slug_id  TEXT NOT NULL,    -- 16-char Base32
        predicate        TEXT NOT NULL,    -- predicate_token (opaque, case-sensitive ASCII)
        target_slug_id   TEXT NOT NULL,    -- 16-char Base32
        timestamp        TEXT NOT NULL,    -- ISO8601 UTC (last upsert)
        entity_id        TEXT NOT NULL,    -- 16-char Base32 (last upsert actor)

        FOREIGN KEY (subject_slug_id) REFERENCES slugs(slug_id),
        FOREIGN KEY (target_slug_id)  REFERENCES slugs(slug_id),
        FOREIGN KEY (entity_id)       REFERENCES entities(entity_id)
    );
```

Semantics:

- Each row is one directed triple `(subject_slug_id, predicate, target_slug_id)`.
- Relationships are authored in Markdown in terms of `slug_id` and projected to instances by pairing with `instance_id` at evaluation time.
- When Markdown authoring uses address identities (Section 5.7), those edges are stored in `address_relationships` instead of `template_relationships`.
- The server stores `predicate` as an opaque token and does not attach semantics; reference daemons interpret the edges (Section 9.5).

#### 6.3.2 Table: `address_relationships`

Address-level relationships are the primary operational relationship form, as defined in Section 3.4.1.

```sql
    CREATE TABLE address_relationships (
        subject_address_id  TEXT NOT NULL,   -- 32-char Base32
        predicate           TEXT NOT NULL,   -- predicate_token (opaque, case-sensitive ASCII)
        target_address_id   TEXT NOT NULL,   -- 32-char Base32
        timestamp           TEXT NOT NULL,   -- ISO8601 UTC (last upsert)
        entity_id           TEXT NOT NULL,   -- 16-char Base32 (last upsert actor)

        FOREIGN KEY (subject_address_id) REFERENCES slugs(address_id),
        FOREIGN KEY (target_address_id)  REFERENCES slugs(address_id),
        FOREIGN KEY (entity_id)          REFERENCES entities(entity_id)
    );
```

Rules:

- Address-level relationships MUST NOT silently rewrite template relationships; they are a separate layer of edges authored in address space.
- A deployment MAY omit this table entirely if it does not use address-level relationships.

#### 6.3.3 Table: `predicates` (Optional Governance)

The `predicates` table tracks vocabulary for relationship predicates without preventing the use of free-text predicates.

```sql
    CREATE TABLE predicates (
        name        TEXT PRIMARY KEY,   -- predicate_token (opaque, case-sensitive ASCII)
        kind        TEXT NOT NULL,      -- e.g. 'canonical', 'custom', 'deprecated'
        status      TEXT NOT NULL,      -- e.g. 'active', 'deprecated'
        description TEXT,
        meta        TEXT                -- optional JSON (aliases, owner, etc.)
    );
```

Rules:

- Relationship rows (`template_relationships`, `address_relationships`) store `predicate` as plain `TEXT`.
- No foreign key from relationships to `predicates` is required by the core spec; deployments MAY enable one after their vocabulary stabilizes.
- The API/MCP layer MAY:
  - accept any predicate token that matches the lexical rules in Section 11.9,
  - warn when the predicate does not appear in `predicates` or is marked `deprecated`,
  - warn when the predicate is not canonical per the reference grammar (Section 9.5.2),
  - still ingest the relationship without failure.
- Free-text predicates are allowed and primarily signal “needs review” or provide hints for downstream tooling.

### 6.4 Table: `history` (Optional Audit Log)

The `history` table records snapshot state changes over time. It is optional but strongly recommended for auditability, review, and analytics.

```sql
    CREATE TABLE history (
        address_id  TEXT NOT NULL,   -- 32-char Base32
        timestamp   TEXT NOT NULL,   -- ISO8601 UTC
        result      TEXT,
        status      TEXT,
        comment     TEXT,
        entity_id   TEXT NOT NULL,   -- 16-char Base32

        FOREIGN KEY (address_id) REFERENCES slugs(address_id)
            ON UPDATE CASCADE
            ON DELETE CASCADE,
        FOREIGN KEY (entity_id) REFERENCES entities(entity_id),

        PRIMARY KEY (address_id, timestamp)
    );
```

Rules:

- Each row is a snapshot of mutable state for one `address_id` at one `timestamp`.
- The primary key `(address_id, timestamp)` ensures unique historical points.
- Snapshots are appended when meaningful changes occur (status, result, comment), not necessarily every read or evaluation.
- `entity_id` records which entity performed the update, resolved via the entity-principal checklist and identity process.

### 6.5 Table: `entities` (Canonical Entity Catalog)

The `entities` table is the canonical catalog of entities that can perform updates (humans, robots, agents, CI jobs, etc.). Every `entity_id` used in `slugs` and `history` MUST appear here.

```sql
    CREATE TABLE entities (
        entity_id    TEXT PRIMARY KEY,   -- 16-char Base32
        principal    TEXT NOT NULL,      -- canonical entity principal string
        kind         TEXT NOT NULL,      -- e.g. 'human', 'robot', 'agent', 'system'
        display_name TEXT,               -- e.g. "J. Smith", "ci-runner-01"
        meta         TEXT                -- optional JSON (auth IDs, roles, etc.)
    );
```

Rules:

- `entity_id` is derived deterministically from the canonical entity principal string (Section 4.5 and 4.6.3).
- `principal` is the canonical UTF-8/NFC, single-line string used as the hash input; it is produced by the Entity Principal Template Checklist (see Section 6.8.2).
- `kind` and `display_name` are deployment-defined and used for display and reporting.
- `meta` MAY contain structured data (e.g. external auth IDs, email, rights, group membership); handling of PII is deployment-specific.
- Authoring-capable clients SHOULD receive `principal`, `kind`, and `display_name` when permitted; deployments MAY redact or omit `principal` for read-only callers in accordance with UI flags (Section 13.1).
- `entities` is REQUIRED; no `slugs` or `history` rows may reference an `entity_id` that is not in this table.

### 6.6 Table: `instance_catalog` (Recommended for Authoring)

Deployments SHOULD expose instance principals for lookup and UI labeling via this catalog or an equivalent adapter over another store. Authoring- and instantiation-capable clients MUST be able to round-trip `instance_principal` even if the catalog is virtual.

```sql
    CREATE TABLE instance_catalog (
        instance_id  TEXT PRIMARY KEY,   -- 16-char Base32
        principal    TEXT NOT NULL,      -- canonical instance principal string
        label        TEXT,               -- human-friendly label (e.g. "Scanner-3")
        meta         TEXT                -- optional JSON (location, serial, etc.)
    );
```

Rules:

- The catalog (or adapter) MUST allow `instance_principal` to be returned to authorized callers; read-only callers MAY receive only `instance_id` or hashed principals per Section 13.
- `principal` is the canonical instance principal string used for hashing; it SHOULD match the content derived from the instance checklist.
- `label` and `meta` are deployment-local and may be omitted or redacted in exports.
- A canonical root/template principal (e.g., `template||default`) SHOULD be seeded and retrievable so clients can target the template instance deterministically.

### 6.7 Indexes

Recommended indexes for performance:

```sql
    CREATE INDEX idx_slugs_slug_id          ON slugs(slug_id);
    CREATE INDEX idx_slugs_instance_id      ON slugs(instance_id);
    CREATE INDEX idx_slugs_checklist        ON slugs(checklist);

    CREATE INDEX idx_trel_subject           ON template_relationships(subject_slug_id);
    CREATE INDEX idx_trel_target            ON template_relationships(target_slug_id);

    CREATE INDEX idx_arel_subject           ON address_relationships(subject_address_id);
    CREATE INDEX idx_arel_target            ON address_relationships(target_address_id);

    CREATE INDEX idx_history_entity         ON history(entity_id);
    CREATE INDEX idx_entities_kind          ON entities(kind);
```

Deployments MAY add further indexes for frequently queried fields (e.g. `section`, `procedure`, or `checklist` + `status` combinations).

### 6.8 Mapping Markdown / Entity Principal Template / API → SQLite

All mutations of the runtime store MUST occur via API/MCP endpoints. Markdown files and the Entity Principal Template Checklist are treated as clients that produce canonical objects and send them through the same write contracts as any other tool.

#### 6.8.1 Markdown Checklist Ingestion

1. **Client-side parsing**
   - Normalize Markdown to UTF-8 NFC and `\n` line endings.
   - Parse `checklist`, `section`, `procedure`, `action`, `spec`, `instructions`, and relationship bullets as defined in Section 5.
   - Create in-memory template rows and template relationships.

2. **ID derivation**
   - Canonicalize template fields and compute `slug_id`.
   - Construct the instance principal string for the deployment’s default instance or a specific instance checklist and compute `instance_id`.
   - Compute `address_id = slug_id || instance_id`.

3. **API/MCP submission**
   - For each `(slug_id, instance_id)` pair:
     - Create or upsert the slug via the **full slug creation contract** or minimal update contract (Section 10).
   - For each template relationship:
     - Call the template-relationship creation endpoint with `(subject_slug_id, predicate, target_slug_id)`.

4. **Server-side behavior**
   - The server validates fields, computes IDs, writes into `slugs`, `template_relationships`, and `history` (if enabled), and returns structured responses.
   - Any state-machine evaluation is read-only and may attach warnings or evaluation flags to responses.

Markdown ingestion is “just another client”: it does not bypass the API, and it uses the same contracts as UI tools, agents, and automation.

#### 6.8.2 Entity Principal Template Checklist (Required)

Entity identity is defined via a unified **Entity Principal Template Checklist**. For each entity (human, robot, agent, CI job, webhook, etc.):

1. An external authentication provider (SSO/OIDC/API-key/CI, etc.) validates credentials and yields a set of validated attributes (issuer, subject, handle, robot-id, fingerprint, roles, etc.).
2. A client or gateway fills in the Entity Principal Template Checklist using these attributes as `result` values for its rows.
3. The entity-principal logic (client or gateway) builds the canonical entity principal string from those checklist values, following the canonicalization rules in Sections 4.5 and 11.4.1.
4. The client computes `entity_id` from the principal string or submits the principal string to the server, which computes `entity_id`.
5. The client calls the API to:
   - ensure the entity’s slugs in `slugs` are present (the principal checklist itself is stored like any other checklist), and
   - upsert a row in `entities` with `(entity_id, principal, kind, display_name, meta)`.

Rules:

- No `entity_id` may appear in `slugs` or `history` unless it has been issued via this entity principal process.
- Authentication remains external: Checklist Assistant does not accept or validate raw passwords, secrets, or OAuth codes. It only canonicalizes already-validated identity attributes.
- Every actor that can update slugs MUST have a corresponding `entity_id` derived from the Entity Principal Template Checklist.

This creates symmetry with instance identity: both instances and entities are identified by principal strings, derived from checklists, and hashed into stable IDs.

### 6.9 Treatment of Updates

State updates via API/MCP:

- identify the target slug by `address_id`
- modify only mutable fields: `result`, `status`, `comment`
- cause the server to:
  - regenerate `timestamp` (current UTC, ISO8601)
  - resolve the active `entity_id` (derived via the entity principal process)
  - write the updated row in `slugs`
  - append a snapshot row in `history` (if enabled)

Rules:

- Addressing fields (`checklist`, `section`, `procedure`, `action`, `spec`, `instructions`, `slug_id`, `instance_id`, `address_id`) MUST NOT be mutated by the minimal update contract.
- Any change to template fields MUST create a new slug (new `slug_id`, `address_id`) via the full creation contract.
- Relationships MUST be created/updated via dedicated relationship endpoints, never by directly mutating relationship tables.
- Internal server-side automation (e.g., state-machine-driven roll-up logic, dependency enforcement) MUST write changes using the same minimal update contract as external clients; internal modules MUST NOT bypass the API semantics when persisting state.

### 6.10 Deletions, Deactivation, and Regeneration

Deletions:

- Slugs SHOULD be soft-deleted by marking them inactive via a checklist or deployment-local field (e.g., a dedicated “Retired” checklist) rather than dropping rows.
- Hard deletion of slugs or entities SHOULD be reserved for administrative cleanup, privacy/PII erasure, or repair of corrupted data.
- When a slug is deleted, template and address-level relationships referencing it SHOULD be removed or marked invalid as part of a maintenance process.

Regeneration:

- Given the complete set of Markdown templates and entity/instance principal checklists, a deployment can rebuild:
  - slugs (template and instance identity),
  - template relationships,
  - entities,
  - optionally instance_catalog.
- Regeneration assumes instance principals remain discoverable through the catalog/adapter (Section 6.6), including the seeded root/template principal.
- `history` is not regenerated from authoring content and MUST be backed up separately if audit trails are required.

### 6.11 SQLite Configuration Notes

-- Recommended SQLite configuration for Checklist Assistant

```sql
PRAGMA encoding = "UTF-8";
PRAGMA journal_mode = WAL;        -- better concurrency
PRAGMA foreign_keys = ON;         -- enforce entities/slugs/history consistency
PRAGMA synchronous = NORMAL;      -- or FULL for maximum durability
```


With appropriate indexing and WAL mode, SQLite can comfortably support hundreds of thousands to millions of slugs, relationships, and history rows for typical Checklist Assistant deployments, while remaining embeddable and easy to back up.

## 7. Transport Interfaces (HTTP API, JSON-RPC, MCP)

This section defines the canonical transport interfaces for Checklist Assistant:

- REST/HTTP+JSON API for general use.
- JSON-RPC for batch and graph/evaluation operations.
- MCP tools for LLM/agent access.

All transports:

- Operate only on the canonical SQLite runtime store (Section 6).
- Respect identity and canonicalization rules (Sections 3–4, 11).
- Use the minimal update and full creation contracts (Section 10).
- Never mutate addressing fields of existing slugs in place.

### 7.1 API Shape, Versioning, Envelopes

#### 7.1.1 Base Paths and Versioning

- REST base path: `/api/v1/`
- JSON-RPC base path: `/rpc/v1`
- MCP tools: deployment-defined, but MUST target v1 semantics.

A deployment MAY expose `/api/v2/`, `/rpc/v2`, etc. Each version MUST:

- be self-contained, and
- not silently alter v1 semantics.

#### 7.1.2 Content Types

- REST and JSON-RPC:
  - `Content-Type: application/json; charset=utf-8`
- JSONL exports (where provided):
  - `Content-Type: application/x-ndjson; charset=utf-8`

#### 7.1.3 Response Envelope (REST and JSON-RPC)

All successful responses MUST use a fixed envelope:

```json
{
  "ok": true,
  "data": { },
  "warnings": []
}
```

- `data` MAY be an object or an array, depending on endpoint.
- `warnings` MAY be omitted if empty.

All errors MUST use:

```json
{
  "ok": false,
  "error": {
    "code": "INVALID_FIELD",
    "message": "Status must be Pass, Fail, NA, or Other",
    "details": {
      "field": "status",
      "allowed": ["Pass","Fail","NA","Other"]
    }
  }
}
```

Rules:

- `code` is a stable, machine-readable string (e.g., `NOT_FOUND`, `INVALID_FIELD`, `UNAUTHORIZED`, `CONFLICT`, `INTERNAL_ERROR`).
- `message` is human-readable, in English by default.
- `details` MAY contain structured context for debugging or UI.

This envelope MUST be used consistently for:

- REST endpoints,
- JSON-RPC method results,
- MCP tool responses (modulo MCP’s own wrapping, if any).

#### 7.1.4 Common Query Parameters

List endpoints MAY accept:

- `limit` (integer, page size, default and max deployment-defined),
- `offset` or `cursor` (for pagination),
- basic filters (e.g., `checklist`, `status`, `kind`).

Pagination behavior MUST be stable and documented per endpoint.

### 7.2 Authentication and Entity Resolution

Checklist Assistant assumes:

- **External authentication** (OAuth/OIDC/SSO/API key/CI token) validates the caller.
- Checklist Assistant receives a set of validated identity attributes (issuer, subject, client, robot-id, etc.), not raw credentials.

#### 7.2.1 Auth Model

- External auth is REQUIRED for normal usage.
- Local API keys MAY be supported for bootstrap and maintenance.
- Checklist Assistant itself:
  - MUST NOT accept or store passwords or raw OAuth codes.
  - MUST treat external auth as the canonical source of “who is calling”.

#### 7.2.2 Entity Principal Resolution

Every authenticated caller MUST map to a stable `entity_id` via the **Entity Principal Template Checklist** (Section 6.8.2):

1. External auth produces a set of validated attributes.
2. A gateway or client fills in the Entity Principal Template Checklist with those attributes as `result` fields.
3. A canonical entity principal string is derived from that checklist.
4. The principal string is canonicalized and hashed to a 16-character `entity_id` (Sections 4.5 and 4.6.3).
5. Checklist Assistant ensures a row exists in `entities` with that `entity_id`.

All write operations:

- MUST resolve the caller to an `entity_id`.
- MUST record that `entity_id` in `slugs` and `history`.

Checklist Assistant MAY expose helper endpoints to assist this process (below), but the identity mapping logic MUST be deterministic and deployment-controlled.

UI sessions MAY surface the resolved `entity_principal` in a session/user menu when `ui.show_entity_principal` is enabled (Section 13.1); high-security deployments default to hiding or hashing it.

### 7.3 REST API: Entities

These endpoints manage and inspect entries in the `entities` catalog (Section 6.5).

#### 7.3.1 POST `/api/v1/entities`

Purpose: ensure an `entity_id` exists for a canonical principal.

Request:

```json
{
  "principal": "idp:azuread || 00000000-0000-0000-0000-000000000000",
  "kind": "human",
  "display_name": "J. Smith",
  "meta": {
    "email": "jsmith@example.com",
    "roles": ["admin"]
  }
}
```

Behavior:

- Canonicalize `principal` per Section 4.5.
- Derive `entity_id` (Sections 4.5, 4.6.3).
- Upsert into `entities`:
  - `entity_id`, `principal`, `kind`, `display_name`, `meta`.
- Return:

```json
{
  "ok": true,
  "data": {
    "entity_id": "ABCDEFGHJKMNPQRS",
    "principal": "...",
    "kind": "human",
    "display_name": "J. Smith",
    "meta": { ... }
  },
  "warnings": []
}
```

Notes:

- If the row already exists with the same `principal` and `entity_id`, this is idempotent.
- If `kind` / `display_name` / `meta` differ, the server MAY update them or MAY reject with `CONFLICT`, depending on deployment policy.

#### 7.3.2 GET `/api/v1/entities/{entity_id}`

Return one entity:

```json
{
  "ok": true,
  "data": {
    "entity_id": "ABCDEFGHJKMNPQRS",
    "principal": "...",
    "kind": "human",
    "display_name": "J. Smith",
    "meta": { ... }
  },
  "warnings": []
}
```

- `NOT_FOUND` if unknown.

#### 7.3.3 GET `/api/v1/entities`

List entities with optional filters:

- `kind` (e.g., `human`, `robot`, `agent`, `system`)
- `limit`, `cursor` or `offset`

Response:

```json
{
  "ok": true,
  "data": {
    "items": [
      { "entity_id": "...", "kind": "...", "display_name": "...", "meta": { } }
    ],
    "next_cursor": "..."
  },
  "warnings": []
}
```

Deleting entities is deployment-specific and MAY be restricted; the core spec does not require a delete endpoint.

### 7.4 REST API: Slugs

Slugs are the central operational object. REST endpoints cover:

- Creation (full slug creation contract).
- Retrieval and listing.
- Minimal state updates.
- Optional bulk operations.

#### 7.4.1 Slug Representation (REST)

REST returns slugs with the canonical shape:

```json
{
  "address_id": "SLUGIDSLUGIDINSTIDINSTID",
  "slug_id": "SLUGIDSLUGIDSLUG",
  "instance_id": "INSTIDINSTIDINS",
  "checklist": "Oven Temperature Checks",
  "section": "Temperature Stability",
  "procedure": "Oven Temperature Test",
  "action": "Verify oven temperature stability",
  "spec": "180–190 °C",
  "instructions": "Preheat the oven ...",

  "result": "182 °C",
  "status": "Pass",
  "comment": "",
  "timestamp": "2025-11-29T15:20:11Z",
  "entity_id": "ABCDEFGHJKMNPQRS"
}
```

#### 7.4.2 POST `/api/v1/slugs`

Create a new slug (full creation contract; see Section 10.8).

Request:

```json
{
  "checklist": "Oven Temperature Checks",
  "section": "Temperature Stability",
  "procedure": "Oven Temperature Test",
  "action": "Verify oven temperature stability",
  "spec": "180–190 °C",
  "instructions": "Preheat the oven ...",

  "instance_principal": "machine || model=Machine_A || serial=1234",

  "result": "",
  "status": "NA",
  "comment": "",

  "relationships": [
    {
      "kind": "template",    // "template" or "address"
      "predicate": "passPropagateValidatedPass",
      "target_slug_id": "1234567890ABCDEF"   // for kind=template
    }
  ]
}
```

Behavior:

1. Canonicalize template fields and compute `slug_id` (Section 4.6.1).
2. Canonicalize `instance_principal` and compute `instance_id` (Section 4.6.2).
3. Compute `address_id = slug_id || instance_id`.
4. Insert into `slugs` if `address_id` does not exist; otherwise:
   - either return `CONFLICT`, or
   - treat as idempotent (deployment-specific, but MUST be documented).
5. Insert any requested relationships:
   - `kind=template` → `template_relationships`.
   - `kind=address`  → `address_relationships` (only allowed if `target_address_id` supplied instead of `target_slug_id`).
6. Set `timestamp` = current UTC.
7. Resolve caller’s `entity_id` and store it.

Response:

```json
{
  "ok": true,
  "data": {
    "address_id": "SLUGIDSLUGIDINSTIDINSTID",
    "slug_id": "SLUGIDSLUGIDSLUG",
    "instance_id": "INSTIDINSTIDINS",
    "timestamp": "2025-11-29T15:20:11Z",
    "entity_id": "ABCDEFGHJKMNPQRS"
  },
  "warnings": []
}
```

#### 7.4.3 GET `/api/v1/slugs/{address_id}`

Fetch a single slug by address:

```json
{
  "ok": true,
  "data": {
    "address_id": "...",
    "slug_id": "...",
    "instance_id": "...",
    "checklist": "...",
    "section": "...",
    "procedure": "...",
    "action": "...",
    "spec": "...",
    "instructions": "...",
    "result": "...",
    "status": "NA",
    "comment": "",
    "timestamp": "2025-11-29T15:20:11Z",
    "entity_id": "ABCDEFGHJKMNPQRS"
  },
  "warnings": []
}
```

- `NOT_FOUND` if unknown.

#### 7.4.4 GET `/api/v1/slugs`

List slugs with filters:

Supported query parameters (non-exhaustive):

- `checklist` (exact match)
- `section` (exact match)
- `status` (`Pass`, `Fail`, `NA`, `Other`)
- `slug_id`
- `instance_id`
- `limit`, `cursor`

Clients SHOULD include `instance_id` when listing slugs for a checklist to avoid mixing multiple instances. If `instance_id` is omitted, servers MAY default to the root/template instance (Section 4.4) or return all instances but MUST emit a warning so callers do not assume a single instance.

Response:

```json
{
  "ok": true,
  "data": {
    "items": [ { ...slug... } ],
    "next_cursor": "..."
  },
  "warnings": []
}
```

#### 7.4.5 PATCH `/api/v1/slugs/{address_id}` (Minimal Update Contract)

Implements the minimal update contract (Section 10.1–10.4).

Request:

```json
{
  "result": "182 °C",
  "status": "Pass",
  "comment": "Stabilized after 12 min"
}
```

Behavior:

1. Lookup slug by `address_id`. If missing → `NOT_FOUND`.
2. Update the provided fields among:
   - `result`
   - `status`
   - `comment`
3. Regenerate `timestamp` (UTC).
4. Resolve `entity_id` from the caller.
5. Write updated row into `slugs`.
6. Append snapshot to `history` (if enabled).
7. Optionally run evaluation (read-only; see 7.7.1) and attach warnings.

Response:

```json
{
  "ok": true,
  "data": {
    "address_id": "SLUGIDSLUGIDINSTIDINSTID",
    "updated_fields": ["result","status","comment"],
    "timestamp": "2025-11-29T15:24:03Z",
    "entity_id": "ABCDEFGHJKMNPQRS"
  },
  "warnings": []
}
```

- Attempts to update addressing fields (`checklist`, `section`, etc.) MUST yield `INVALID_FIELD`.

#### 7.4.6 POST `/api/v1/slugs/bulk-update`

Bulk minimal updates:

```json
{
  "updates": [
    {
      "address_id": "SLUG...INST...",
      "status": "Pass",
      "result": "OK"
    },
    {
      "address_id": "SLUG...INST...",
      "status": "Fail",
      "comment": "Leak detected"
    }
  ]
}
```

Response:

```json
{
  "ok": true,
  "data": {
    "results": [
      {
        "address_id": "SLUG...INST...",
        "ok": true,
        "updated_fields": ["status","result"],
        "timestamp": "2025-11-29T15:24:03Z",
        "entity_id": "ABCDEFGHJKMNPQRS",
        "warnings": []
      },
      {
        "address_id": "SLUG...INST...",
        "ok": false,
        "error": {
          "code": "NOT_FOUND",
          "message": "Slug not found",
          "details": { "address_id": "..." }
        }
      }
    ]
  },
  "warnings": []
}
```

This endpoint MUST treat each update independently; partial failure is allowed.

### 7.5 REST API: Relationships

Both template and address-level relationships are available via REST for simple clients. Graph and evaluation operations SHOULD prefer JSON-RPC (Section 7.8).

#### 7.5.1 POST `/api/v1/relationships/template`

Create a template-level relationship triple.

Request:

```json
{
  "subject_slug_id": "1234567890ABCDEF",
  "predicate": "customReviewRequired",
  "target_slug_id": "234567890ABCDEF1"
}
```

Behavior:

- Validate:
  - `subject_slug_id` and `target_slug_id` format and existence.
  - `predicate` token (Section 11.9).
- Insert row into `template_relationships`, recording `timestamp` and caller `entity_id`.
- If `predicate` not in `predicates` table or marked deprecated, add a `warnings` entry; ingestion MUST still succeed.

Response:

```json
{
  "ok": true,
  "data": {
    "subject_slug_id": "1234567890ABCDEF",
    "predicate": "customReviewRequired",
    "target_slug_id": "234567890ABCDEF1"
  },
  "warnings": [
    {
      "code": "UNKNOWN_PREDICATE",
      "message": "Predicate 'customReviewRequired' not registered in predicates table",
      "details": { "predicate": "customReviewRequired" }
    }
  ]
}
```

#### 7.5.2 POST `/api/v1/relationships/address`

Create an address-level relationship triple (deployments may disable).

Request:

```json
{
  "subject_address_id": "SLUGA...INSTA...",
  "predicate": "passSyncValidatedPass",
  "target_address_id": "SLUGB...INSTB..."
}
```

Behavior:

- Validate address IDs, existence of both slugs, and predicate token.
- Insert into `address_relationships`, recording `timestamp` and caller `entity_id`.
- Same warning behavior for unknown/extension predicates as 7.5.1.

#### 7.5.3 GET `/api/v1/relationships/template`

List template-level relationships with optional filters:

Query parameters:

- `subject_slug_id`
- `target_slug_id`
- `predicate`
- `limit`, `cursor`

Response:

```json
{
  "ok": true,
  "data": {
    "items": [
      {
        "subject_slug_id": "1234567890ABCDEF",
        "predicate": "customReviewRequired",
        "target_slug_id": "234567890ABCDEF1"
      }
    ],
    "next_cursor": null
  },
  "warnings": []
}
```

#### 7.5.4 GET `/api/v1/relationships/address`

List address-level relationships:

Query parameters:

- `subject_address_id`
- `target_address_id`
- `predicate`
- `limit`, `cursor`

Response analogous to template relationships.

#### 7.5.5 GET `/api/v1/relationships/address/{address_id}`

Combined view for a specific address:

```json
{
  "ok": true,
  "data": {
    "address_id": "SLUG...INST...",
    "outgoing": [
      { "predicate": "BoolVerifyValidatedStatus", "target": "..." }
    ],
    "incoming": [
      { "predicate": "passPropagateValidatedPass", "source": "..." }
    ]
  },
  "warnings": []
}
```

This endpoint MUST be read-only. Template-level incoming/outgoing views are available through `GET /api/v1/relationships/template` filters and through export/migration tooling, not through this address-specific endpoint in the current C++ reference implementation.

### 7.6 REST API: History and Instance Catalog

#### 7.6.1 GET `/api/v1/history/{address_id}`

Return snapshots for a given address:

Query parameters:

- `since` (ISO8601, optional)
- `until` (ISO8601, optional)
- `limit`, `cursor`

Response:

```json
{
  "ok": true,
  "data": {
    "items": [
      {
        "address_id": "SLUG...INST...",
        "timestamp": "2025-11-29T15:20:11Z",
        "result": "182 °C",
        "status": "Pass",
        "comment": "",
        "entity_id": "ABCDEFGHJKMNPQRS"
      }
    ],
    "next_cursor": null
  },
  "warnings": []
}
```

#### 7.6.2 GET `/api/v1/instances/{instance_id}` (Catalog/Adapter)

When the instance catalog or equivalent adapter is enabled, allow inspecting an instance:

```json
{
  "ok": true,
  "data": {
    "instance_id": "INSTIDINSTIDINS",
    "principal": "machine || model=Machine_A || serial=1234",
    "label": "Scanner-3",
    "meta": {
      "room": "201",
      "building": "Lab"
    }
  },
  "warnings": []
}
```

Authorized callers SHOULD receive `principal`; deployments MAY redact or hash it for read-only callers according to `ui.show_instance_principal` (Section 13.1).

#### 7.6.3 GET `/api/v1/instances`

List instances (catalog or adapter enabled), with filters:

- `label` (substring match, deployment-defined)
- `limit`, `cursor`

#### 7.6.4 GET `/api/v1/workspace/markdown/templates` (Workspace Discovery)

When local workspace access is enabled, this endpoint lists Markdown checklist templates under the checklist asset root and pre-parses each `checklist.md` before an author imports it.

Response entries SHOULD include enough metadata for UI and agent clients to make a safe load decision: `source_name`, `source_path`, pack, checklist folder name, relative path, `valid`, parsed checklist name, discovered slug count, template relationship count, machine-readable `warnings`, and a parse `error` when invalid.

Authoring UIs SHOULD surface this preflight state directly in the template picker, for example with a compact success/warning/error indicator and a details view for warnings. This keeps malformed relationship lines, ambiguous authoring shortcuts, Unicode/report escaping problems, and other ingest diagnostics visible before the user commits the import.

Workspace import, export, report, script, and list operations SHOULD accept `source_name`, `pack`, and `checklist_dir` when the caller knows the selected workspace item. If those fields are omitted and multiple persisted ownership matches exist for the same checklist/instance, the reference behavior is to return an ambiguity error and include candidate owners rather than silently picking one.

#### 7.6.5 POST `/api/v1/workspace/asset-pack/export` and `/api/v1/workspace/asset-pack/import`

Transportable asset-pack archives move a complete `checklists/<pack>/<checklist>/` folder as one file. The reference implementation recognizes `.chk`, `.7z`, and `.zip`; `.chk` is a 7-Zip-format archive with a Checklist Assistant-specific extension.

Archive export accepts `source_name`, `pack`, `checklist` or `checklist_dir`, optional `output_path`, and optional `format` (`chk`, `7z`, or `zip` when `output_path` is omitted). It writes an archive containing the pack/checklist folder structure.

Archive import accepts `archive_path`, optional `source_name`, optional `pack` and `checklist_dir` overrides, `replace_files`, and the same template/apply options as workspace Markdown import. It extracts the archive into a temporary staging directory, restores each detected checklist folder into the selected checklist source, then imports the restored `checklist.md` through the normal workspace Markdown import path.

Archive import stores the runtime checklist model in SQLite: slugs/rows, template and derived address relationships, address IDs for the selected instance principal, order metadata, and source/pack/checklist ownership metadata. Non-row assets remain filesystem assets under the restored checklist folder, including `data/`, `media/`, `templates/`, `scripts/`, `docs/`, `reports/`, `saves/`, and `logs/`.

### 7.7 REST API: Evaluation (Read-Only)

Evaluation endpoints are optional and **strictly read-only**: they MUST NOT perform database writes. The current C++ reference implementation exposes Verify diagnostics for `BoolVerify...` relationships.

#### 7.7.1 GET `/api/v1/evaluate/slug/{address_id}`

Evaluate Verify relationships for one address.

Response:

```json
{
  "ok": true,
  "data": {
    "address_id": "SLUG...INST...",
    "effective_status": "Pass",
    "verify": [
      {
        "predicate": "BoolVerifyValidatedStatus",
        "target_address_id": "SLUGB...INST...",
        "predicate_bool": "true",
        "reason_code": "OK",
        "reason": "",
        "would_write": false,
        "write_decision": "WRITE_STATUS_PASS_NO_CHANGE",
        "gate_applied": false,
        "gate_mode": "",
        "contributor_count": 1,
        "contributor_true_count": 1
      }
    ],
    "flags": [
      {
        "code": "VERIFY_NO_WRITE",
        "details": {
          "predicate": "BoolVerifyValidatedStatus",
          "target_address_id": "SLUGB...INST...",
          "write_decision": "WRITE_STATUS_PASS_NO_CHANGE"
        }
      }
    ]
  },
  "warnings": []
}
```

Notes:

- All evaluation outputs are NON-authoritative unless written back through the API update contracts.
- Clients/daemons MAY choose to apply writes via the minimal update contract, but this endpoint MUST NOT apply writes.

#### 7.7.2 POST `/api/v1/evaluate/graph`

Evaluate a small graph around given addresses.

Request:

```json
{
  "root_address_ids": [
    "SLUGA...INSTA...",
    "SLUGB...INSTB..."
  ],
  "max_depth": 3,
  "include_incoming": true,
  "include_outgoing": true
}
```

Response:

```json
{
  "ok": true,
  "data": {
    "nodes": [
      {
        "address_id": "SLUGA...INSTA...",
        "effective_status": "Pass",
        "verify": [],
        "flags": []
      }
    ]
  },
  "warnings": []
}
```

This endpoint is intended for visualization, diagnostics, and agent reasoning.

### 7.8 JSON-RPC API

JSON-RPC is used for:

- Batch operations (slugs, relationships, ingestion results).
- Graph/evaluation operations.
- Advanced queries that would otherwise need multiple REST calls.

#### 7.8.1 JSON-RPC Envelope

Requests:

```json
{
  "jsonrpc": "2.0",
  "method": "slug.bulk_upsert",
  "params": { },
  "id": "request-123"
}
```

Responses:

```json
{
  "jsonrpc": "2.0",
  "id": "request-123",
  "result": {
    "ok": true,
    "data": { },
    "warnings": []
  }
}
```

Errors:

```json
{
  "jsonrpc": "2.0",
  "id": "request-123",
  "error": {
    "code": -32000,
    "message": "Application error",
    "data": {
      "ok": false,
      "error": {
        "code": "INVALID_FIELD",
        "message": "Status must be Pass, Fail, NA, or Other",
        "details": { "field": "status" }
      }
    }
  }
}
```

The Checklist Assistant error envelope is nested in `error.data`.

#### 7.8.2 `slug.bulk_upsert`

Batch creation/update of slugs using full or minimal contracts.

Parameters:

```json
{
  "items": [
    {
      "mode": "create",   // or "update"
      "payload": {
        "checklist": "...",
        "section": "...",
        "procedure": "...",
        "action": "...",
        "spec": "...",
        "instructions": "...",
        "instance_principal": "...",
        "result": "",
        "status": "NA",
        "comment": "",
        "relationships": []
      }
    },
    {
      "mode": "update",
      "payload": {
        "address_id": "SLUG...INST...",
        "status": "Pass",
        "result": "OK"
      }
    }
  ]
}
```

Result:

```json
{
  "ok": true,
  "data": {
    "results": [
      {
        "mode": "create",
        "ok": true,
        "address_id": "SLUG...INST...",
        "timestamp": "2025-11-29T15:30:00Z",
        "entity_id": "ABCDEFGHJKMNPQRS",
        "warnings": []
      },
      {
        "mode": "update",
        "ok": false,
        "error": {
          "code": "NOT_FOUND",
          "message": "Slug not found",
          "details": { "address_id": "..." }
        }
      }
    ]
  },
  "warnings": []
}
```

#### 7.8.3 `relationship.bulk_upsert`

Batch upsert for template/address relationships.

Parameters:

```json
{
  "items": [
    {
      "kind": "template",
      "subject_slug_id": "1234567890ABCDEF",
      "predicate": "passPropagateValidatedPass",
      "target_slug_id": "234567890ABCDEF1"
    },
    {
      "kind": "address",
      "subject_address_id": "SLUGA...INSTA...",
      "predicate": "passSyncValidatedPass",
      "target_address_id": "SLUGB...INSTB..."
    }
  ]
}
```

Result:

```json
{
  "ok": true,
  "data": {
    "results": [
      { "ok": true, "kind": "template" },
      { "ok": true, "kind": "address" }
    ]
  },
  "warnings": []
}
```

#### 7.8.4 `graph.evaluate`

Graph evaluation equivalent to `/api/v1/evaluate/graph`, but optimized for agent use.

Parameters:

```json
{
  "root_address_ids": ["SLUGA...INSTA..."],
  "max_depth": 3,
  "include_incoming": true,
  "include_outgoing": true
}
```

Result uses the same shape as 7.7.2, inside the standard envelope.

#### 7.8.5 `ingest.apply`

Optional method for applying **pre-parsed** ingest batches (e.g., a Markdown ingestor that already computed slugs on the client, or that runs in a separate “ingest” container).

Parameters:

```json
{
  "source": "markdown",
  "items": [
    {
      "checklist": "...",
      "section": "...",
      "procedure": "...",
      "action": "...",
      "spec": "...",
      "instructions": "...",
      "instance_principal": "...",
      "relationships": [ ... ]
    }
  ]
}
```

Semantics:

- MUST use the same rules as `POST /api/v1/slugs` and relationship endpoints.
- MUST NOT bypass the slug/relationship validation pipeline.
- Intended for batch use only; simple clients SHOULD use REST.

### 7.9 MCP Tools

MCP tools are logical wrappers around the same runtime operations. They expose an LLM/agent-friendly schema over the same semantics and MUST call the HTTP API (or an equivalent API adapter) rather than directly reading or mutating the runtime store.

The current native `chax-mcp-bridge` tool catalog is tracked in `docs/mcp_tools.md`. The required interoperability surface is:

- Discovery and smoke tools: `chax.list_commands`, `chax.health`, `chax.hello`, `chax.echo`.
- Checklist and slug reads: `chax.list_checklists`, `chax.list_slugs`, `chax.get_slug`, `chax.get_checklist`.
- Minimal writes: `chax.update_slug`.
- Relationship tools: `chax.relationships`, `chax.list_template_relationships`, `chax.create_template_relationship`, `chax.list_address_relationships`, `chax.create_address_relationship`.
- Catalog tools: `chax.create_entity`, `chax.list_entities`, `chax.create_instance`, `chax.list_instances`.
- Evaluation tools: `chax.evaluate_slug`, `chax.evaluate_graph`; these MUST remain read-only.
- Import/export tools: `chax.export_json`, `chax.export_markdown`, `chax.import_markdown`.

Tool names and JSON Schemas should remain stable once exposed. Breaking changes require a documented version bump, an update to `docs/mcp_tools.md`, and a CHANGELOG entry.

### 7.10 Error Codes and Warnings

Common `error.code` values:

- `INVALID_FIELD`
- `MISSING_FIELD`
- `NOT_FOUND`
- `CONFLICT`
- `UNAUTHORIZED`
- `FORBIDDEN`
- `RATE_LIMITED`
- `INTERNAL_ERROR`

Warnings (non-fatal) MAY use:

- `UNKNOWN_PREDICATE`
- `DEPRECATED_PREDICATE`
- `MISSING_RELATIONSHIP_TARGET`
- `CYCLE_DETECTED`
- `INDETERMINATE_STATUS`

Warnings MUST never change database state by themselves; they are advisory.

### 7.11 Invariants

The transport layer MUST uphold:

- **Identity immutability**:
  - Addressing fields cannot be changed in place.
  - New addressing → new slug via full creation contract.
- **Minimal update scope**:
  - Only `result`, `status`, `comment` are writable via minimal update.
  - `timestamp` and `entity_id` are regenerated server-side.
- **Single authority**:
  - All mutations go through REST or JSON-RPC, including server-internal automation.
- **Read-only evaluation**:
  - Evaluation endpoints never write; all state changes are explicit updates.
- **Determinism**:
  - Identical inputs (template fields, principals) produce identical IDs and results across deployments.

This completes the canonical API, JSON-RPC, and MCP surface for Checklist Assistant (the `chax.*` namespace).

### 7.x Authentication and Entity Resolution

When a request is authenticated, the server resolves an **acting entity**.

Resolution order:

1. If the request explicitly provides `entity_principal`:
   - Server MUST validate it matches the authenticated identity.
   - Server derives `entity_id` from that principal.

2. Else if authentication context provides a stable identity (e.g. OAuth subject):
   - Server MUST derive `entity_principal` implicitly.
   - Server MUST compute `entity_id` from it.

3. Else (unauthenticated or guest mode):
   - Server uses a deployment-defined default entity principal (e.g. `guest||provider=none`).

At no point are OAuth tokens or secrets persisted.

## 8. Relationship Model

The Checklist Assistant relationship model defines how checklist slugs refer to each other and how these references are stored, transported, and evaluated.

Relationships are:

- Authored in Markdown at the **template** level.
- Stored in SQLite as normalized triples.
- Exposed over REST, JSON-RPC, and MCP.
- Interpreted by clients/daemons (Section 9) according to deployment policy; the server core does not interpret predicate semantics.

The model distinguishes:

- **Template-level relationships**: between `slug_id`s.
- **Address-level relationships**: between `address_id`s.
- **Predicates**: opaque tokens that clients/daemons may interpret.

### 8.1 Goals

The relationship model is designed to:

- Make dependencies and roll-ups explicit and machine-readable.
- Preserve portability across deployments by modeling relationships at the template level.
- Allow deployments to add **address-specific operational wiring** without changing template content.
- Support a reference predicate grammar and evaluation techniques while still allowing deployment-defined predicate tokens without hard failure.
- Keep the server core simple: storage and evaluation are well-defined; any additional automation remains a client/extension concern using the API.

### 8.2 Conceptual Graph

At the conceptual level Checklist Assistant maintains two related directed graphs:

1. **Template graph** (mandatory):

Nodes:
   - `slug_id` (template identity for checklist rows).

Edges:
   - Triples of the form:

     ```text
     (subject_slug_id, predicate, target_slug_id)
     ```

This graph is **portable** across deployments and instances.

2. **Address graph** (operational):

Nodes:
   - `address_id` (composite of `slug_id || instance_id`).

Edges:
   - Triples:

     ```text
     (subject_address_id, predicate, target_address_id)
     ```

This graph is **deployment-local** and encodes day-to-day operational wiring, including cross-instance edges.

Both graphs:

- Are directed multigraphs (multiple edges between same nodes allowed).
- May contain cycles (Section 9 describes how evaluation handles them).
- Use the same predicate vocabulary.

### 8.3 Template Relationships

Template relationships are the primary, canonical mechanism for expressing how procedures relate logically at design time.

#### 8.3.1 Definition

A template relationship is a triple:

```text
(subject_slug_id, predicate, target_slug_id)
```

Where:

- `subject_slug_id` is the `slug_id` of the row that **declares** the relationship.
- `predicate` is an opaque, case-sensitive ASCII predicate token (Section 11.9).
- `target_slug_id` is the `slug_id` being referenced.

Semantics:

- They describe the **intended structural and logical relationships** between procedures, independent of any specific instance.
- At runtime they are lifted into instance contexts by pairing each `slug_id` with one or more `instance_id`s.

For a given instance `INST_X`, the conceptual evaluation concerns:

```text
(SUBJECT_SLUG_ID, INST_X) --predicate--> (TARGET_SLUG_ID, INST_X)
```

#### 8.3.2 Storage

Template relationships are stored in a dedicated table:

```sql
CREATE TABLE template_relationships (
    subject_slug_id  TEXT NOT NULL,
    predicate        TEXT NOT NULL,     -- predicate_token (opaque, case-sensitive ASCII)
    target_slug_id   TEXT NOT NULL,
    timestamp        TEXT NOT NULL,     -- ISO8601 UTC (last upsert)
    entity_id        TEXT NOT NULL,     -- 16-char Base32 (last upsert actor)

    -- optional FK constraints if desired:
    -- FOREIGN KEY(subject_slug_id) REFERENCES slugs(slug_id),
    -- FOREIGN KEY(target_slug_id)  REFERENCES slugs(slug_id)
);
```

Recommended indexes:

```sql
CREATE INDEX idx_template_rels_subject  ON template_relationships(subject_slug_id);
CREATE INDEX idx_template_rels_target   ON template_relationships(target_slug_id);
CREATE INDEX idx_template_rels_predicate ON template_relationships(predicate);
```

The `slugs` table remains keyed by `address_id`; `slug_id` is redundant but indexed for joining with `template_relationships`.

#### 8.3.3 Authoring in Markdown

Template relationships are authored using `slug_id` targets or the same-checklist shorthand `(section, procedure)`, as defined in Section 5.7:

```markdown
#### Relationships
- BCDFGHJKMNPQRSTV
- BoolVerifyValidatedStatus ABCDEFGHJKMNPQRS
- passPropagateValidatedPass Z234567Z234567Z2
```

Rules:

- The first bullet line under `#### Relationships` is the source identity line (`slug_id` or `address_id`) and is not stored as a relationship.
- Each remaining bullet line describes **one relationship**.
- The target may be any accepted form defined in Section 5.7.
- If the source identity line uses address identity (`address_id`), ingest SHOULD store the relationships in `address_relationships` (Section 8.4); otherwise, ingest SHOULD store them in `template_relationships`.
- `predicate` MUST match the predicate token lexical rules (Section 11.9).

On ingest:

- `subject_slug_id` is the computed `slug_id` for the current procedure row when authoring template relationships.
- `subject_address_id` is the provided or computed address when authoring address relationships.
- Each relationship bullet becomes one row in the appropriate table.
- Tuple targets resolve to slug_id matches within the same checklist; ambiguous matches MUST emit a warning or error. Deployments MAY attach to all matches or reject the relationship.

Malformed lines:

- MUST be ignored with a warning, without failing the entire file.
- SHOULD be surfaced to authors (e.g., via logs or UI).

### 8.4 Address-Level Relationships

Address-level relationships describe relationships between **specific instances** of procedures—i.e., between specific `address_id`s.

#### 8.4.1 Definition

An address-level relationship triple is:

```text
(subject_address_id, predicate, target_address_id)
```

Where:

- `subject_address_id` = `subject_slug_id || subject_instance_id`.
- `target_address_id`  = `target_slug_id  || target_instance_id`.

Semantics:

- They are a separate operational layer of edges; they MUST NOT silently rewrite the template graph.
- They are used for deployment-local wiring such as:
  - pairing instances across rooms or systems,
  - expressing that one machine’s check depends on a **different** machine’s check,
  - modeling cross-instance interactions not encoded at the template level.

Examples:

- “Instance A of Cube must be paired with Instance B of Cube before step Y passes.”
- “Room 201’s HVAC verification depends on Room 101’s main plant state.”

#### 8.4.2 Storage

Address-level relationships use a separate table:

```sql
CREATE TABLE address_relationships (
    subject_address_id  TEXT NOT NULL,
    predicate           TEXT NOT NULL,   -- predicate_token (opaque, case-sensitive ASCII)
    target_address_id   TEXT NOT NULL,
    timestamp           TEXT NOT NULL,   -- ISO8601 UTC (last upsert)
    entity_id           TEXT NOT NULL,   -- 16-char Base32 (last upsert actor)

    FOREIGN KEY(subject_address_id) REFERENCES slugs(address_id),
    FOREIGN KEY(target_address_id)  REFERENCES slugs(address_id)
);
```

Recommended indexes:

```sql
CREATE INDEX idx_address_rels_subject  ON address_relationships(subject_address_id);
CREATE INDEX idx_address_rels_target   ON address_relationships(target_address_id);
CREATE INDEX idx_address_rels_predicate ON address_relationships(predicate);
```

This separation ensures:

- Template semantics remain independent of specific deployments.
- Address-level overlays are easily discoverable and can be enabled/disabled, migrated, or pruned deployment-by-deployment.

#### 8.4.3 Creation and Management

Address relationships are typically created via:

- REST: `POST /api/v1/relationships/address`
- JSON-RPC: `relationship.bulk_upsert` with `kind="address"`
- MCP: tools that call the same backend functionality using the `chax.*` namespace; MCP is a descriptive wrapper so agents reuse the same HTTP/JSON-RPC surface rather than a separate API.

Canonical Markdown describes *template-level* relationships keyed by `slug_id`, and cross-instance wiring keyed by `address_id`. Address overlays are enabled by default and MAY be authored either through the APIs above, through structured overlay files, or by emitting explicit `address_id` references (e.g., via `@address` hints) inside Markdown. Smaller deployments can disable address-level ingest entirely, but the baseline specification keeps it on to ensure cross-instance graphs are preserved. Regardless of authoring surface, persist those relationships in a companion JSONL/CSV/DB overlay derived from the deterministic `address_id`s so they are not lost when Markdown is regenerated.

### 8.5 Predicate Tokens and Canonical Grammar

`predicate` is a **predicate token** stored with each relationship triple. The server core treats it as an opaque, case-sensitive ASCII identifier and does not attach semantics (Sections 9.5 and 11.9).

The reference implementation defines a canonical predicate grammar for deterministic parsing by daemons and clients (Section 9.5.2). Deployments MAY define additional custom predicate tokens; the reference daemon is under no obligation to parse or act on non-canonical tokens.

### 8.6 Predicates Catalog (Optional Governance)

Deployments MAY maintain a `predicates` catalog to support:

- autocomplete and validation UX,
- deprecation/alias tracking,
- warnings for unknown or non-canonical tokens.

The catalog does not prevent storing arbitrary predicate tokens. See Section 6.3.3 for the reference schema; typical runtime behavior is:

- If a token is not in the catalog, the relationship is still stored and returned.
- The API MAY return warnings such as `UNKNOWN_PREDICATE` or `NON_CANONICAL_PREDICATE`.

### 8.7 Authoring vs Runtime Responsibilities

#### 8.7.1 Authoring (Markdown and Tools)

Authors are responsible for:

- choosing meaningful predicate tokens,
- preferring canonical predicate tokens when they want the reference daemons to act deterministically,
- avoiding contradictory relationship definitions.

#### 8.7.2 Runtime (Server Core)

The server core is responsible for:

- storing and retrieving template- and address-level relationships,
- enforcing lexical constraints on predicate tokens and IDs,
- exposing relationships via REST, JSON-RPC, and MCP,
- attributing relationship upserts via `entity_id` and `timestamp`.

The server core is explicitly not responsible for interpreting predicate semantics.

#### 8.7.3 External Logic (Daemons, Clients, Extensions)

Reference daemons and deployment-specific automation:

- pull relationships via the API,
- interpret predicate tokens (canonical and/or custom),
- write back only via the standard API contracts (slug creation, minimal updates, relationship upserts), attributed via `entity_id`.

### 8.8 Interaction With Reference Daemons

The relationship graphs are inputs to the reference daemons (Section 9):

- Layer A interprets canonical predicate tokens using the single-trigger model and may apply explicit writes for `Propagate`/`Sync` (Section 9.5).
- Layer B computes optional readiness/health signals above Layer A semantics (Section 9.6).

### 8.9 Examples

#### 8.9.1 Simple Dependency Chain

Markdown:

```markdown
### Procedure: Pre-check
- Action: Verify prerequisites
- Spec: All pre-checks complete
- Result:
- Status:
- Comment:

#### Instructions
Ensure all upstream checks are complete.

#### Relationships
- BCDFGHJKMNPQRSTV
- passPropagateValidatedPass ABCDEFGHJKMNPQRS
```

Assuming `ABCDE...` is the slug_id of another row (“Valve Leak Test”), ingest produces:

```text
(subject_slug_id = SLUG_PRECHECK,
 predicate        = "passPropagateValidatedPass",
 target_slug_id   = "ABCDEFGHJKMNPQRS")
```

At runtime for instance `INST_X` (after projection into address space):

```text
(SLUG_PRECHECK || INST_X) passPropagateValidatedPass (ABCDEFGHJKMNPQRS || INST_X)
```

Evaluation:

- If the subject is `Pass` but the target is `Fail`, the predicate evaluates to `ActiveFalse` and Layer B may mark the subject as `Blocked` or `WithReview` depending on deployment policy.

#### 8.9.2 Cross-Instance Pairing

Address-level relationship:

```json
{
  "subject_address_id": "SLUGA...INST_201...",
  "predicate": "passSyncValidatedPass",
  "target_address_id": "SLUGA...INST_101..."
}
```

This says: “The same procedure slug A in Room 201 is paired with slug A in Room 101.” Checklist Assistant stores this in `address_relationships` and exposes it for evaluation and visualization. Because `passSyncValidatedPass` is canonical, the reference daemon may enforce stored `status` equality between these two addresses when the predicate is Active.

#### 8.9.3 Extension Predicate

Markdown:

```markdown
#### Relationships
- JKMNPQRSTVWXYZ23
- related_to Z234567Z234567Z2
```

If `related_to` is not in the `predicates` catalog:

- Ingest stores the triple.
- Response (or logs) may include `UNKNOWN_PREDICATE`.
- The reference daemon ignores `related_to` by default (non-canonical), but tools can still use it for navigation or documentation.

---

The relationship model thus provides:

- A clear, portable template graph and a deployment-local operational address graph.
- Opaque predicate tokens for storage/transport, plus a canonical grammar for deterministic reference behavior.
- Extensibility for deployments to add custom predicate tokens and logic without breaking portability of the underlying triples.

## 9. Relationship Predicate Semantics and Evaluation Model

This section defines the canonical relationship predicate grammar (reference implementation), the reference evaluation model, and the explicit separation of concerns between:

- checklist state (`slugs`),
- relationship logic (Layer A), and
- aggregation logic (Layer B).

The server core stores relationship triples and predicate tokens but does **not** interpret predicate semantics. All semantic interpretation (and any resulting state mutations) is performed by external clients/daemons operating against the API, with every write attributed via `entity_id`.

### 9.1 Goals

This model exists to:

- Keep the core checklist state and canonical slug/address identity semantics stable.
- Enable deterministic, portable relationship storage and export/import independent of daemon state.
- Provide a canonical predicate grammar and evaluation model for the reference daemons.
- Allow deployments to extend predicate vocabularies without breaking storage or portability.

### 9.2 Scope and Non-Goals

**In scope:**

- Stable storage and transport of relationship triples.
- Lexical rules for predicate tokens (Section 11.9).
- Canonical predicate grammar for reference parsing (Section 9.5.2).
- Deterministic single-trigger evaluation semantics for canonical predicates (Layer A).
- A basic, non-core aggregation model for readiness signals (Layer B).

**Out of scope:**

- Interpreting semantics for arbitrary custom predicate tokens.
- Global logical completeness, fixpoint inference, or theorem-proving.
- Automatic resolution of conflicting evidence (beyond reference conflict handling for `Sync`).
- Any direct database writes outside the API mutation contracts.

### 9.3 Evaluation Inputs and Outputs

#### 9.3.1 Inputs

For a given evaluation run, a reference daemon consumes:

- Stored slug state by `address_id` (primarily `status`).
- Relevant entries in:
  - `template_relationships` (projected into address space as needed), and
  - `address_relationships` (if enabled).
- Optional governance inputs:
  - `predicates` (catalog), and
  - daemon configuration (write limits, sweep limits).

#### 9.3.2 Outputs

Reference daemons may produce read-only diagnostic outputs (for UIs/agents), including:

- Per-edge evaluation state: `ActiveTrue`, `ActiveFalse`, or `Inactive` (Section 9.5.3).
- For `Verify` predicates: a boolean `predicate_bool` signal (Section 9.5.4).
- For `Propagate`/`Sync` predicates: explicit proposed writes (never implicit writes via read-only endpoints).
- Layer B results per target row: `readiness` and `rule_health` (Section 9.6).

All such outputs are **non-authoritative** unless and until they are written back through the normal API update contracts (Section 10), attributed to a specific `entity_id`.

### 9.4 Evaluation Unit and Terminology

The basic evaluation unit is a **slug instance** identified by:

```text
address_id = slug_id || instance_id
```

For each `address_id` we consider:

- Stored fields:

  - `status`, `result`, `comment`, `timestamp`, `entity_id`.

- Template-level relationships:

  - All triples with `subject_slug_id = this slug_id`.
  - Lifted to this instance via `instance_id`.

- Address-level relationships:

  - All triples with `subject_address_id = this address_id`.

A read-only evaluation pass is purely functional: same inputs → same outputs. Applying writes (e.g., `Propagate`/`Sync`) is a separate, explicit step performed via the API contracts.

### 9.5 Relationship Predicate Semantics and Evaluation (Layer A)

#### 9.5.1 Relationship Storage Model

Relationships are stored as triples:

```text
(subject_address_id, predicate_token, target_address_id)
(subject_slug_id, predicate_token, target_slug_id)
```

- The server treats `predicate_token` as an opaque, case-sensitive ASCII string (Section 11.9).
- The server does **not** interpret predicate semantics.
- All semantic interpretation is performed by clients or daemons operating against the API, so no component must “reach around” the API to directly mutate the runtime database.
- Relationship upserts are attributed via `entity_id` and a `timestamp` (Section 6.3).

#### 9.5.2 Canonical Predicate Grammar (Reference Implementation)

The reference implementation defines canonical predicate grammar families to enable deterministic parsing by daemons and clients.

The status propagation family is constructed by concatenating four semantic slots in **lowerCamelCase**:

```text
<subjectState><relation><type><objectState>
```

Status-family slot values recognized by the reference parser:

| Slot | Meaning | Recognized Values |
|---|---|---|
| `subjectState` | Activating stored `status` on the subject address | `pass` / `fail` / `other` / `na` |
| `relation` | Relationship behavior | `Propagate` / `Sync` / `Verify` |
| `type` | Relationship type (epistemic and/or gate behavior) | `Validated` / `Implied` / `Assumed` / `AndGate` / `OrGate` |
| `objectState` | Intended stored `status` for the target address | `Pass` / `Fail` / `Other` / `Na` |

Examples:

- `passPropagateValidatedPass`
- `failPropagateAssumedFail`
- `passSyncValidatedPass`
- `passPropagateAndGatePass`
- `passPropagateOrGatePass`

The deterministic `spec`/`result` verification family uses the explicit `BoolVerify<Type><Object>` form, where `Bool` is a virtual subject derived from evaluating the subject row's `spec` against its `result`. Recognized type values are `Validated`, `Implied`, `Assumed`, `AndGate`, and `OrGate`. Recognized object values are `Status`, `Comment`, or a status literal (`Pass`, `Fail`, `Other`, `Na`). Examples:

- `BoolVerifyValidatedStatus`
- `BoolVerifyAndGateStatus`
- `BoolVerifyOrGateStatus`
- `BoolVerifyValidatedComment`
- `BoolVerifyValidatedPass`

The slot-field family uses the same four-slot shape for field copy and lookup behavior:

```text
<subjectFieldOrStatus><relation><type><objectField>
```

Slot-field values recognized by the reference parser:

| Slot | Meaning | Recognized Values |
|---|---|---|
| `subjectFieldOrStatus` | Activating subject field, or a status filter applied to the subject row | `Result` / `Status` / `Comment` / `Section` / `Action` / `Spec` / `Procedure` / `Instructions` / `Timestamp` / `pass` / `fail` / `other` / `na` |
| `relation` | Field behavior | `Search` / `Propagate` |
| `type` | Field behavior type | `Prefill` / `Validated` |
| `objectField` | Target field addressed by the behavior | `Result` / `Status` / `Comment` / `Section` / `Action` / `Spec` / `Procedure` / `Instructions` / `Timestamp` |

Examples:

- `ResultSearchPrefillResult`
- `ResultSearchPrefillComment`
- `ResultSearchPrefillStatus`
- `PassSearchPrefillResult`
- `ResultPropagateValidatedResult`

Predicate tokens:

- MUST be ASCII and case-sensitive
- SHOULD match one of the canonical grammar families above
- MAY be custom-defined by deployments

The reference daemon is under **zero obligation** to parse or act on non-canonical predicate tokens. Current C++ write-capable behavior implements status-family `Propagate`, deterministic `BoolVerify...`, slot-field `SearchPrefill`, and slot-field `PropagateValidated` paths. `Sync` and status-family `...Verify...` tokens remain grammar-compatible extension points unless a deployment enables a daemon that acts on them. New deterministic verification authoring SHOULD use the `BoolVerify...` family rather than status-family `...Verify...` tokens.

Reference implementation hooks: predicate parsing and write planning live in `src/core/checklist_store.cpp`; browser-side predicate validation and authoring hints live in `CHAX-CLIENT/web/checklist_prototype_common.js`; HTTP and exhaustive predicate coverage lives in `tests/http_api_test.cpp`, `tests/predicate_daemon_exhaustive_test.cpp`, and `tests/predicate_daemon_exhaustive_csv_test.cpp`. These references are intentionally file-level hooks rather than line anchors so the spec does not drift on ordinary refactors.

#### 9.5.3 Single-Trigger Predicate Model (Layer A)

Status-family canonical predicates are **single-trigger** rules.

For a given relationship predicate:

- The predicate is **Active** if and only if the subject address’ stored `status` matches the `subjectState` slot.
- All other canonical predicates on the same subject address are **Inactive**.
- Inactive predicates are ignored and are not evaluated as false.

Canonical state-word mapping to stored `status` values:

- `pass` / `Pass` → `Pass`
- `fail` / `Fail` → `Fail`
- `other` / `Other` → `Other`
- `na` / `Na` → `NA`

##### Predicate Evaluation States

Each predicate has a derived evaluation state:

| State | Meaning |
|---|---|
| `ActiveTrue` | Predicate is active and the target matches expected `objectState` |
| `ActiveFalse` | Predicate is active and the target does not match expected `objectState` |
| `Inactive` | Subject stored `status` does not match `subjectState` |

Inactive predicates are not included in any aggregation or readiness logic by default.

#### 9.5.4 Relationship Types (Layer A Semantics)

Type handling in the reference implementation:

```text
Validated > Implied > Assumed
```

`AndGate` and `OrGate` are gate types (slot 3) used to aggregate multiple contributors that share the same target and full predicate token.

##### Propagate

- **Direction:** subject → target
- **Behavior (reference daemon):** when Active, daemon writes the target’s stored `status` to `objectState` via the minimal update contract
- **Provenance:** write attributed to the daemon’s `entity_id`

Propagate behavior by type:

- `Validated` / `Implied` / `Assumed`: deterministic 1:1 propagation.
- `AndGate`: write only when all matching incoming contributors for that target+predicate are Active.
- `OrGate`: write when at least one matching incoming contributor for that target+predicate is Active.

Missing contributors (for example, orphaned relationships pointing to unloaded/missing sources) are treated as inactive by the reference daemon and should produce warnings rather than hard errors.

##### Verify

- **Direction:** subject → target
- **Behavior:** evaluates the subject row's `spec` against the subject row's `result`; no state is written by read-only evaluation endpoints
- **Output:** produces a boolean signal `predicate_bool` with values `true`, `false`, or `indeterminate`
- **Bridge writes:** write-capable reference-daemon behavior is expressed with `BoolVerify<Type><Object>` tokens, for example `BoolVerifyValidatedStatus`
- **Purpose:** supports deterministic `spec`/`result` checks and optional bridge writes without embedding a general expression language in checklist rows

The reference verifier recognizes deterministic `spec`/`result` forms in this order:

1. Numeric comparators: `<=`, `>=`, `<`, `>`, `==`, `!=`, `=`.
2. Inclusive dot ranges: `a..b`.
3. Bracket/parenthesis interval notation: `[a,b]`, `(a,b)`, `[a,b)`, `(a,b]`.
4. Boolean tokens: `true/false`, `yes/no`, `y/n`, `pass/fail`.
5. Scalar equality.
6. Case-insensitive text equality fallback.

Scalar parsing accepts a numeric value plus an optional unit suffix. Unit suffixes may contain letters, digits, `/`, `_`, `-`, `^`, and `%`. Unit conversion is implemented for mapped dimensions such as pressure, length, current, voltage, time, power, flow, dose, charge, charge-per-area, current-per-solid-angle, and percent. Unitless results may adopt the spec unit when the value is otherwise unambiguous.

Equality (`=` / `==` and scalar equality) uses a spec-precision tolerance window when the spec encodes precision through fractional digits or exponent notation. The current policy is a quarter-step of the least significant specified place after conversion to canonical units.

Preferred quantitative authoring forms:

- Comparator: `<= 5 s`, `= 24 V`, `!= 10`.
- Inclusive range: `10 mm..12 mm`.
- Explicit interval: `[10 mm,12 mm]`, `(10 mm,12 mm]`.
- Compatibility interval: `[10 - 12] mm`.

Bare dash ranges such as `10-12 mm` remain unsupported because they are ambiguous. Chained comparator expressions in one `spec` remain unsupported; authors should use supported range grammar or split out-of-band checks into multiple rows connected with `AndGate`/`OrGate` relationships. Strict `qty:` authoring is not the recommended path for new checklists; interval notation and explicit units are the normative authoring style for deterministic quantitative checks.

Verify gate types mirror gate intent for evaluation: `AndGate` requires all matching contributors to evaluate true; `OrGate` requires any matching contributor to evaluate true.

Qualitative rows use the manual-first path rather than forcing synthetic parsing. A human, agent, or LLM selecting `Pass` asserts the row is true for downstream status-driven propagation; selecting `Fail` asserts false. `NA` and `Other` are non-boolean outcomes and should be treated as `indeterminate` in bool-bridge contexts unless a deployment-specific authoring policy says otherwise.

Default bridge behavior:

- `BoolVerify<Type>Status`: `true -> status=Pass`, `false -> status=Fail`, `indeterminate -> no status/comment write`.
- `BoolVerify<Type>Comment`: write literal `TRUE`, `FALSE`, or `INDETERMINATE`.
- `BoolVerify<Type>Pass|Fail|Other|Na`: true-gated behavior; write the configured object state only when `predicate_bool == true`.

Result entry remains warning-first and non-blocking. If a `spec` is deterministic but the current `result` is missing or unparsable, evaluation should return `indeterminate` with diagnostics rather than preventing the user from entering free-form text.

##### Slot-Field Prefill and Propagation

- **Direction:** subject -> target
- **Field propagation:** `ResultPropagateValidatedResult` and related `PropagateValidated` tokens copy a supported mutable source field to a supported mutable target field through the normal update contract.
- **Prefill lookup:** `SearchPrefill` tokens search checklist-local CSV data for a row matching the subject field or status filter, then write the requested target field through the normal update contract.
- **Writable target fields:** the current reference implementation writes runtime fields such as `result`, `status`, `comment`, and prefill `timestamp`; identity/template fields such as `section`, `procedure`, `action`, `spec`, and `instructions` are parsed for addressing and lookup but are not treated as ordinary runtime write targets.
- **Activation:** field-triggered prefill runs when the relevant subject field changes; status-filter subjects such as `PassSearchPrefillResult` run when the subject row is in the corresponding status.
- **Data locality:** prefill datasets belong beside the checklist under the checklist asset root so the checklist, scripts, reports, and lookup data move together.

Slot-field predicates are the current built-in mechanism for lightweight checklist-specific lookup logic. They are preferable to one-off custom daemons when the behavior is a deterministic lookup or field copy expressible as data.

##### Sync

- **Direction:** bidirectional
- **Behavior:** enforces equality of stored `status` between subject and target
- **Trigger:** either side becoming Active
- **Conflict resolution (reference implementation):**
  1. Higher epistemic `type` wins (`Validated` > `Implied` > `Assumed`)
  2. If equal type priority, last-write-wins by timestamp
  3. If unresolved, flag `AttentionRequired` and stop further writes for that connected component

Sync predicates allow stakeholder-specific wording redundancy without duplicating operational work.

The current C++ server seeds `passSyncValidatedPass` as a known predicate but does not apply Sync writes in the built-in predicate sweep. Deployments that require Sync behavior must enable a compatible daemon or extension that still writes through the normal update contract.

#### 9.5.5 Cascade Control and Daemon Write Limits

The reference predicate daemon operates as a standard system actor identified by a stable `entity_id`.

To prevent runaway cascades:

- The daemon MUST NOT apply a write to any row whose last `entity_id` equals the daemon’s own `entity_id`, unless explicitly enabled by configuration.
- The daemon performs a single evaluation sweep per detected external update and does not attempt fixpoint inference.

These limits are mechanical safeguards, not epistemic judgments.

### 9.6 Layer B Aggregation (In-Spec, Non-Core)

Aggregation logic operates **above** predicate semantics and does not modify canonical predicate behavior.

The reference implementation MAY include a basic Layer B aggregation analyzer/daemon to demonstrate functionality.

#### 9.6.1 Aggregation Inputs

For a given target address, Layer B may consume:

- `ObjectState`: the current stored `status` of the target address
- `predicate_bool`: evaluation outcome of active `Verify` predicates (Section 9.5.4)
- `type`: the associated predicate type

#### 9.6.2 Reference Aggregation Outputs

For a given target address:

**Readiness**

- What constitutes a “required” relationship is deployment policy (for example: only `Validated` predicates, or only a named subset of predicates).
- **Blocked:** any required relationship predicate evaluation yields `ActiveFalse`
- **WithReview:** any required relationship yields `Other`, or any required relationship has weak type (`Assumed`), or a deployment-defined “review-needed” condition is met
- **Satisfied:** otherwise

**RuleHealth**

- **OK:** no `ActiveFalse` predicates exist
- **AttentionRequired:** one or more `ActiveFalse` predicates exist

The reference implementation may allow an aggregation daemon to record these results in comments or derived fields for user visibility, attributed to the daemon’s `entity_id`.

Aggregation policies beyond this basic model (for example nested AND/OR trees, precedence graphs, temporal logic, etc.) are intentionally out of scope for the core and left to deployment-specific extensions.

#### 9.6.3 Scope Boundary Summary

This specification guarantees:

- Stable relationship storage and transport
- Canonical predicate grammar (for the reference implementation)
- Deterministic single-trigger semantics for canonical predicates (Layer A)
- Portable exports independent of daemon state

This specification intentionally does **not** guarantee:

- Global logical completeness
- Boolean aggregation correctness beyond the reference model
- Automatic resolution of conflicting evidence

### 9.7 Template-Level vs Address-Level Relationships

- **Template-level (slug-based) relationships** are authored by checklist designers and typically change slowly.
- **Address-level (instance-based) relationships** are intended for day-to-day operational use and may be authored by non-author entities.

Both are first-class within the specification. Deployments MAY restrict permissions for template-level relationships while enabling broad use of address-level relationships. Proven, high-value address-level relationships may be promoted back into templates by authors.

### 9.8 Optional Time-Based Rules

Some deployments may want time-aware evaluation (e.g., checks expire after a certain period). Time-based logic is **optional** and must be configured explicitly.

Examples (non-normative):

- If `timestamp` older than X days:

  - Add `STALE` flag.
  - Set derived readiness to `WithReview` (or produce an equivalent review-needed signal).

- If windowed tests are required (e.g., daily checks):

  - If no history entries in the last 24 hours → `DUE` flag.

Constraints:

- Time-based rules must be **pure evaluation**:

  - No auto-writing to `status`.
  - No implicit creation of new `history` rows.

- Any actual change (e.g., marking something Fail due to staleness) must be performed via API update calls initiated by a client or automation service.

### 9.9 Cycles and Conflicts

The relationship graph may contain cycles and conflicting constraints. Reference daemons must detect and surface these issues; they never attempt to silently “fix” them.

Common cases:

- **Cycles:** a connected component contains a path that returns to the same address via relationships considered in a daemon run.
- **Conflicting writes:** multiple active `Propagate`/`Sync` rules attempt to drive the same target to different `objectState` values.
- **Ambiguous evidence:** a `Sync` component contains incompatible states with equal type priority and no safe resolution.

Reference handling:

- Produce explicit diagnostics (flags or `AttentionRequired`), including the involved `address_id`s and predicate tokens.
- For write-capable behavior (`Propagate`/`Sync`), stop further writes for the affected component when a conflict is detected.
- Do not mutate checklist state solely to “make the graph consistent”; any mutation must remain an explicit result of applying a recognized rule through the normal API update contracts.

### 9.10 Evaluation Interfaces

Evaluation/analysis interfaces are optional. Deployments MAY expose them via the same transport mechanisms as other operations (HTTP+JSON, JSON-RPC, MCP), either as a standalone daemon service or as an integrated component that still behaves like a normal API client for any writes.

#### 9.10.1 HTTP+JSON

Reference endpoints:

- `GET /api/v1/evaluate/slug/{address_id}`
- `POST /api/v1/evaluate/graph`

  - Graph request:

    ```json
    {
      "root_address_ids": ["AAA...", "BBB..."]
    }
    ```

  - Response:

    ```json
    {
      "nodes": [
        {
          "address_id": "AAA...",
          "effective_status": "Pass",
          "verify": [],
          "flags": []
        }
      ]
    }
    ```

- Deployments may also expose checklist-wide or system-wide evaluation endpoints (e.g. `GET /api/v1/evaluate/checklist/<name>`).

#### 9.10.2 JSON-RPC

A JSON-RPC method like:

```json
{
  "method": "chax.evaluate",
  "params": {
    "checklist": "oven_check"
  },
  "id": 1
}
```

Returning the same shape as the REST example.

#### 9.10.3 MCP

An MCP tool, e.g. `chax.evaluate`, with schema:

- Inputs:

  - `checklist` (optional).
  - `address_ids` (optional list).
  - `include_relationships` (boolean).

- Outputs:

  - An array of evaluation results (`address_id`, Layer A/LB outputs, flags).

This allows agents to:

- Fetch only the relevant slice of graph into context.
- Explain why a slug is blocked or indeterminate.
- Suggest or apply updates (via separate update/create tools) if authorized.

### 9.11 Interaction With Updates and History

The server core is the canonical mutation gateway and stores only checklist state and relationship triples. Reference daemons interact with updates and history as follows:

- When a state update is applied via API (Section 10):
  1. The server updates `slugs`.
  2. If enabled, a new `history` snapshot is appended.
  3. Daemons observe the change (polling, subscription, or batch evaluation) and compute Layer A/Layer B outputs.

- If a daemon chooses to apply a write (for example, `Propagate` or `Sync`):
  - It MUST do so via the minimal update contract.
  - It MUST attribute the write via its own stable `entity_id`.
  - It MUST respect the cascade limits in Section 9.5.5.

- Diagnostic evaluation outputs MAY be returned by optional evaluation endpoints and/or logged externally, but they are not required to be persisted in SQLite.

### 9.12 Extensibility and Versioning

Deployments may extend relationship behavior by:

- Defining additional predicate tokens and maintaining a local `predicates` catalog.
- Implementing additional daemons or evaluation passes that:
  - Read the same underlying tables,
  - Produce additional diagnostics or derived metrics, and
  - Never bypass the API contracts for writes.

Any extension that writes to `slugs`, `history`, or relationship tables MUST:

- Use the same minimal update / creation / relationship upsert contracts as any other client.
- Be clearly documented as external automation and attributable via `entity_id`.

This separation keeps the core stable and predictable while allowing deployment-specific sophistication through deterministic, explicit mechanisms.

## 10. Write Contracts (Creation, Minimal Update, Batch Ingest)

Checklist Assistant’s write model enforces strict, minimal, predictable update semantics. All state changes—whether made by a human, script, automation service, or agent—use **the same contracts**. This prevents “hidden state mutation” and keeps the system auditable.

There are three write surfaces:

1. **Slug Creation Contract**
2. **Minimal Update Contract**
3. **Batch Ingest Contract** (Markdown, JSONL, operational data)

No other write pathways exist. REST, JSON-RPC, and MCP all dispatch to these contracts.

---

### 10.1 Principles

All write operations MUST obey these invariants:

#### **10.1.1 No mutation of addressing fields**  
Once created, the following properties of a slug are immutable:

- `slug_id`
- `instance_id`
- `address_id = slug_id || instance_id`
- `checklist_slug` (derived or explicit)
- All template-derived structural fields

#### **10.1.2 Updates are *minimal***  
A minimal update:

- Only changes the requested fields (`status`, `result`, `comment`).
- Does not modify other fields.
- Automatically appends a `timestamp` if provided or derived.
- Immediately writes a `history` record (if enabled).

#### **10.1.3 Creation is explicit**  
A slug must be fully created before it can be updated. Creation:

- Inserts a full row in `slugs`.
- Must provide all required fields.

#### 10.1.4 Idempotency

Idempotency applies to the *semantic effect* of a minimal update, not to the timestamp alone.

A minimal update is considered **idempotent** when:

1. The caller supplies the same set of fields (`status`, `result`, `comment`) with values identical to the currently stored state, **and**
2. The update occurs within the deployment’s configured **idempotency window** Δt.

Within this window:

- The server MUST treat the update as a duplicate and MUST NOT create a new history row.
- The server MUST NOT modify `timestamp`.
- The server MUST return a response indicating no effective change (e.g., `updated_fields = []`).

Outside this window:

- The update MAY be applied even if fields match the stored values.
- The server MUST generate a new `timestamp` and append a new history snapshot.
- `updated_fields` SHOULD be empty, but the timestamp change is still a legitimate update that reflects “confirmation at a later time.”

##### Default Idempotency Window

Deployments SHOULD default Δt to **several seconds** (e.g., 5–10 seconds). This avoids spurious history spam from rapidly repeated UI or automation calls, while allowing normal repeated confirmations at meaningful time intervals.

##### Deployment Configuration

Deployments MAY adjust Δt globally. Any configurable behavior MUST follow these rules:

- The default window MUST apply unless explicitly overridden.
- Operators SHOULD NOT be exposed to this configuration in normal use.
- Per-entity or per-instance overrides MUST be treated as **advanced administrative configuration** and MUST NOT be the default behavior for general deployments.
- Overrides MUST NOT silently change behavior for existing slugs; they apply only prospectively.

##### Future Extensions

If future versions permit:

- per-instance overrides of Δt, or
- per-entity idempotency policies,

then implementations MUST:

- preserve the global default unless explicitly configured otherwise,
- document overrides clearly,
- ensure overrides do not break audit expectations or confuse standard users.

The overriding principle remains:

> “Idempotency prevents repeated identical updates within a short window from producing spurious state changes, while still allowing later confirmations to update timestamps and history in a controlled, predictable way.”

#### **10.1.5 Evaluation is not a write**  
Evaluation (Section 9) never performs creations or updates—it only computes derived state.

---

### 10.2 Slug Creation Contract

Slug creation inserts a **new address_id** row into `slugs`.

#### 10.2.1 Required Fields

Clients MUST provide:

- `slug_id` (template identity)
- `instance_id` (instance identity)
- `address_id` = computed concatenation
- `checklist_slug`
- `section`
- `procedure`
- `action`
- `spec`
- `instructions`

Clients MAY also provide initial:

- `status`
- `result`
- `comment`
- `entity_id` (who created)
- `timestamp`

#### 10.2.2 REST Example

**Request:**

```json
{
  "slug_id": "ABCDEFGHJKMNPQRS",
  "instance_id": "2025-oven-01",
  "checklist_slug": "oven_startup",
  "section": "preflight",
  "procedure": "vent_check",
  "action": "verify_valve",
  "spec": "Valve is closed",
  "status": "",
  "result": "",
  "comment": ""
}
```

**Response:**

```json
{
  "ok": true,
  "data": {
    "address_id": "ABCDEFGHJKMNPQRS||2025-oven-01"
  },
  "warnings": []
}
```

#### 10.2.3 Error Conditions

Creation MUST fail if:

- `address_id` already exists.
- `slug_id` malformed.
- `instance_id` malformed.
- Required fields missing.
- Invalid field formats.

#### 10.2.4 Instantiation workflow (template reuse)

Authoring clients SHOULD follow a consistent flow when creating a new checklist instance without duplicating template identity:

1. List template rows for the root/template principal (`template||default`) to obtain `slug_id` values for each row.
2. For each template row, call `POST /api/v1/slugs` with the target `instance_principal`; the server reuses the template `slug_id` and derives a new `instance_id` and `address_id`.
3. Perform subsequent `PATCH` calls using `(slug_id, instance_id)` or `address_id` under the minimal update contract.
4. List calls SHOULD include `instance_id`; when omitted, servers use the root/template principal or return a warning (Section 7.4.4).

This keeps template identity stable while allowing multiple instances to exist without address collisions.

---

### 10.3 Minimal Update Contract

The minimal update contract is one of the core invariants of Checklist Assistant (CHAX).

#### 10.3.1 Updatable Fields

Only three fields are updatable:

- `status`
- `result`
- `comment`

The server MUST reject updates that attempt to change:

- `slug_id`
- `instance_id`
- `address_id`
- `checklist_slug`
- `section`, `procedure`, `action`, `spec`
- Any legacy/derived fields

#### 10.3.2 Timestamp Behavior

- If client provides `timestamp`, it MUST be ISO 8601.
- If absent, server MUST generate a timestamp at update time.

#### 10.3.3 History Behavior

If history is enabled:

- Every minimal update MUST append a new row in `history`.
- The history row captures:
  - `address_id`
  - `status`, `result`, `comment` (full snapshot)
  - `entity_id`
  - `timestamp` (snapshot time)
- No deltas; always full snapshots.

#### 10.3.4 REST Example

**Request:**

```json
{
  "status": "Pass",
  "comment": "Valve verified closed"
}
```

**Response:**

```json
{
  "ok": true,
  "data": {
    "address_id": "ABCDEFGHJKMNPQRS||2025-oven-01",
    "stored_status": "Pass"
  },
  "warnings": []
}
```

#### 10.3.5 JSON-RPC Example

```json
{
  "method": "chax.slug.update",
  "params": {
    "address_id": "ABCDEFGHJKMNPQRS||2025-oven-01",
    "status": "Fail",
    "comment": "Valve leaking"
  },
  "id": 12
}
```

---

### 10.4 Relationship Upsert Contract

Applies to:

- Template relationships
- Address relationships

#### 10.4.1 Allowed Changes

Upserts may:

- Insert a new relationship triple.
- Replace a triple (if identical subject and predicate, but new target).
- Delete a relationship triple (via REST DELETE or JSON-RPC op).

#### 10.4.2 Forbidden Changes

Cannot:

- Mutate `slug_id`, `address_id`, or any slug fields.
- Automatically infer relationships.
- Create slugs.

#### 10.4.3 REST Example (Template Relationship)

**POST /api/v1/relationships/template**

```json
{
  "subject_slug_id": "ABCDEFGHJKMNPQRS",
  "predicate": "BoolVerifyValidatedStatus",
  "target_slug_id": "Z234567Z234567Z2"
}
```

#### 10.4.4 Address Relationship Example

**POST /api/v1/relationships/address**

```json
{
  "subject_address_id": "SLUGA||INST1",
  "predicate": "passSyncValidatedPass",
  "target_address_id": "SLUGA||INST2"
}
```

Predicate tokens MUST be stored as-is. Deployments MAY warn if a token is unknown (not in a catalog) or non-canonical (does not match the reference grammar).

---

### 10.5 Slug Deletion Contract (Optional, Deployment-Specific)

Checklist Assistant core does not require DELETE. Deployments MAY enable deletion via an explicit capability flag; when disabled, servers MUST return `FEATURE_DISABLED`.

When enabled, two optional endpoints become available:

- `DELETE /api/v1/checklists/{checklist}`: removes all slugs for the checklist across instances and clears history for those addresses.
- `DELETE /api/v1/checklists/{checklist}/instances/{instance_id}`: removes all slugs and history for that instance of the checklist.

Invariants when deletion is enabled:

- Template and address-level relationships referencing removed slugs MUST be pruned so no dangling edges remain.
- History rows MUST be removed or marked deleted alongside the slug row to avoid orphaned audit data.
- Address-level relationships MUST be removed with their slugs; template relationships that point to deleted `slug_id`s MUST also be removed or flagged as invalid.
- Clients SHOULD confirm intent (UI confirmation or equivalent) before invoking the endpoint.

Deployments MAY keep deletion disabled by default; soft-delete strategies (marking inactive) remain supported via Section 6.10.

---

### 10.6 Batch Minimal Update Contract (JSON-RPC)

Batch updates allow efficient automation.

#### 10.6.1 Format

```json
{
  "method": "chax.batch.update",
  "params": {
    "updates": [
      {
        "address_id": "AAA||X",
        "status": "Pass"
      },
      {
        "address_id": "BBB||X",
        "status": "Fail",
        "comment": "Leak detected"
      }
    ]
  },
  "id": 99
}
```

Rules:

- Each update is validated independently.
- Batch MUST be atomic per-update, not per-batch. Failures for one update do not stop others from being applied.
- History rows must be inserted for each successful update.

---

### 10.7 Batch Ingest Contract (Markdown / JSONL)

Markdown ingest (templates) and JSONL ingest (state from external systems) both must obey the same safety rules.

#### 10.7.1 Markdown Ingest

Markdown → structured checklist template → API calls:

- Create missing slugs (creation contract).
- Upsert template relationships.
- Optional update of template metadata.

Markdown ingest MUST NOT:

- Directly modify database tables.
- Change existing slugs’ addressing fields.
- Overwrite stored `status`, `result`, or `comment`.

#### 10.7.2 JSONL State Ingest

Each JSONL row represents a minimal update:

```jsonl
{"address_id":"AAA||X","status":"Pass","comment":"OK"}
{"address_id":"BBB||X","status":"Fail"}
```

Processing:

- For each row:
  - Validate fields.
  - Apply minimal update.
  - Append history (if enabled).

#### 10.7.3 Ingest Mode Options

Deployments may offer an ingest mode:

- `strict` Unknown predicates or malformed lines → hard fail.

- `permissive` Unknown predicates allowed, warnings produced.

- `dry_run` Validate but do not mutate.

---

### 10.8 Entity and Instance Creation Contracts

Entity creation and instance creation both use the **slug creation contract**.

#### 10.8.1 Entity Principal Checklist

An entity (person, robot, system) is created by materializing an instance of the **Entity Principal Template Checklist**.

This instance:

- Produces a deterministic `entity_id` from its fields.
- Uses the same minimal update/creation rules.
- Is stored in `slugs` and fully auditable.

The server MUST NOT have a separate privileged “create user” function.

#### 10.8.2 Instance Principal Checklist

Instance creation for:

- Rooms,
- Physical assets,
- Machines,
- Scenarios,

also uses a dedicated template.

Every instance is created by:

1. Selecting appropriate template.
2. Filling required fields.
3. Producing deterministic `instance_id`.
4. Creating slugs for all template rows.

This ensures that instances and entities follow the same lifecycle.

---

### 10.9 Write Contract Error Model

Errors follow the unified envelope (Section 7.1.3).

Examples:

- `ADDRESS_ID_EXISTS`
- `INVALID_FIELD`
- `FORBIDDEN_MUTATION`
- `UNKNOWN_PREDICATE`
- `MISSING_REQUIRED_FIELD`
- `INVALID_TIMESTAMP_FORMAT`
- `RELATIONSHIP_CONFLICT`

---

### 10.10 Validation and Integrity Constraints

Before applying writes, server MUST:

- Validate IDs (Crockford Base32, lengths).
- Validate predicate syntax.
- Validate JSON field types.
- Reject structural mutations.

Server SHOULD:

- Warn on unknown predicates.
- Warn on deprecated predicates.
- Optionally warn on template/address relationship conflicts.

---

### 10.11 Interaction with State Machine

After any update or creation:

1. Database is mutated (minimal update or creation).
2. History snapshot is appended.
3. Evaluation MAY be invoked and returned in the API response.
4. Stored state remains unchanged except for the update itself.

Evaluation MUST NOT:

- Trigger additional updates.
- Create or delete slugs.
- Create relationships.

---

### 10.12 Summary of Contracts

| Operation                 | Allowed? | Fields Affected | History? | Notes |
|--------------------------|----------|-----------------|-----------|-------|
| Slug Creation           | Yes      | All required fields | Optional | Creates new address_id |
| Minimal Update          | Yes      | status/result/comment | Yes | No structural mutation |
| Relationship Upsert     | Yes      | relationship tables | No | Template and address both |
| Evaluate                | No write | None | No | Derived only |
| Delete Slug (optional) | Deployment | Soft-delete | Optional | No cascade |
| Batch Update           | Yes | status/result/comment | Yes | Atomic per-item |
| Markdown Ingest        | Yes | Creates slugs, upserts relationships | Maybe | Never overwrites stored status |
| JSONL Ingest           | Yes | status/result/comment | Yes | Minimal updates only |

---

The write contracts in this section guarantee:

- Consistency across all tools and clients.
- Predictable and auditable changes.
- No hidden logic or privileged mutation paths.
- Easy automation pipelines that rely on stable, minimal rules.

## 11. Identifier System (Canonical ID Model)

Checklist Assistant uses a unified identifier system to ensure:

- Deterministic addressability of all checklist content.
- Stability of references across storage, transmission, and rendering.
- Collisions are extremely unlikely under normal deployment scales.
- Interoperability across REST, JSON-RPC, MCP, and external tools.
- Cross-deployment data exchange without ambiguity.

This section defines **all ID types**, their **formats**, and their **derivation rules**.

---

### 11.1 Overview of ID Types

Checklist Assistant defines five major identifier classes:

1. **Checklist ID** — identifies a checklist template.
2. **Slug ID** — identifies a *template row* (section/procedure/action/spec/instructions).
3. **Instance ID** — identifies a specific *instantiation of a checklist*.
4. **Address ID** — identifies a specific slug *within* a specific instance.
5. **Entity ID** — identifies an agent (user, robot, system) performing updates.

Two auxiliary identifier domains:

- **Predicate tokens** (relationship vocabulary)
- **Checksum (optional)** for future extensions

The reference implementation uses **strict defaults** (Section 11.3) while allowing flexibility for future versions or other deployments (Section 11.10).

---

### 11.2 Canonical Encoding Format

Unless otherwise stated, all IDs follow:

- **Crockford Base32**, uppercase:

  ```
  ABCDEFGHJKMNPQRSTVWXYZ23456789
  ```

- **Exclusions:** Letters `I`, `L`, `O`, `U` MUST NOT appear in ID payloads or examples. (These characters are excluded by the Base32 alphabet for visual safety.)

- **Padding:** No `=` padding MUST be used.

- **Case:** ID strings MUST be strictly uppercase.

- **Separator Rules:** IDs themselves contain **no separators**. Composite structures (e.g., address_id) define their own textual separators.

---

### 11.3 Reference Implementation Defaults (RDI-A)

The reference implementation included with Checklist Assistant uses:

| ID Type       | Length | Checksum | Notes |
|---------------|--------|----------|-------|
| `slug_id`     | 16     | No       | Deterministic from template fields |
| `instance_id` | 16     | No       | Deterministic from instance template |
| `entity_id`   | 16     | No       | Deterministic from entity principal template + salt |
| `address_id`  | —      | —        | Concatenation: `slug_id || instance_id` |
| `checklist_id`| 16     | No       | Deterministic from checklist slug |

Separator for `address_id` (reference default):

```
address_id = slug_id || "||" || instance_id
```

This separator is chosen for readability and ease of parsing.

**Note:** Future revisions MAY adopt longer lengths, optional checksums, or alternative separators. See Section 11.10.

**UI note:** The default Crockford Base32 alphabet remains copy-friendly, but modern clients are expected to provide one-click copy affordances. Deployments that select denser alphabets or different entropy (Section 11.10) MUST keep ID formats consistent across REST/MCP/clients and expose a single-click copy control for IDs.

---

### 11.4 Checklist ID

Each checklist template receives a stable ID derived from:

```
(checklist_slug)
```

#### 11.4.1 Derivation

Procedure:

1. Normalize the checklist slug (lowercase ASCII, no spaces).
2. Hash with a deterministic algorithm (e.g., SHA-256).
3. Extract the required number of bits to produce a 16-character Base32 string.
4. Encode using Crockford Base32, uppercase.

This produces a stable `checklist_id`.

#### 11.4.2 Constraints

- MUST be exactly 16 characters in the reference implementation.
- MUST NOT be regenerated if the file path changes.
- MAY be regenerated if the canonical checklist slug changes.

Checklist slug renaming is a template-level breaking change.

---

### 11.5 Slug ID

A `slug_id` uniquely identifies a *template row*.

#### 11.5.1 Derivation Inputs

Slug ID is derived from the tuple:

```
(checklist_slug, section, procedure, action, spec, instructions)
```

All six fields MUST be present. `instructions` is a required slug field and part of the identity tuple, so even purely textual instruction changes force a new `slug_id` rather than silently mutating behavior in place.

#### 11.5.2 Derivation Procedure

1. Normalize each field:
   - Trim whitespace
   - Collapse internal whitespace
   - Convert to lowercase for hashing (the actual stored text remains original)
2. Concatenate fields with a delimiter known only to the hashing function.
3. Compute a deterministic hash (e.g., SHA-256).
4. Extract bits for a 16-character Base32 ID.
5. Encode uppercase.

#### 11.5.3 Slug ID Immutability

- Any change to the defining fields MUST produce a new `slug_id`.
- Slug IDs MUST NOT be manually edited.

---

### 11.6 Instance ID

An `instance_id` designates a *specific instantiation of a checklist*.

For example:

- A room (Room 201)
- A system (System-01)
- A procedure run (“Oven Startup – Batch 2025-03-05”)

#### 11.6.1 Derivation Inputs

Instance IDs are derived from the **Instance Principal Template fields**.

Examples:

- Room name
- Equipment serial
- Run date
- Configuration parameters

#### 11.6.2 Derivation Procedure

Reference implementation:

1. Collect stable instance-identifying fields.
2. Normalize text (lowercase for hashing).
3. Hash deterministically (SHA-256).
4. Extract 16 Base32 characters.

#### 11.6.3 Notes

- Deployments MAY add additional entropy or fields.
- Length MAY be extended in a future version.

---

### 11.7 Entity ID

Entity IDs identify users, robots, CI systems, or any agent applying updates.

#### 11.7.0 Entity Principal Model (Normative)

An **Entity Principal** is the stable, human-meaningful identity of an actor (user, service, robot, automation) responsible for updates.

It is distinct from authentication credentials.

##### Principles

- Authentication tokens (passwords, OAuth codes, access tokens, refresh tokens) MUST NOT be used as inputs to Entity ID derivation.
- Entity identity MUST be stable across sessions and logins.
- Entity Principal strings MAY be stored, but MUST NOT be exposed to unauthorized clients.
- Entity ID is the only identifier exposed in checklist rows and history.

##### Entity Principal String

Entity IDs are derived from a canonical **Entity Principal String**, composed of stable identity attributes.

Logical form:

entity_type=user username=jds auth_provider=local|oidc|oauth

Serialized canonical form:

"user||username=jds||provider=local"

Robots and services follow the same structure:

"service||name=state_machine||provider=internal"

##### Required Fields by Entity Type

| Entity Type | Required Fields |
|------------|----------------|
| user       | username, auth_provider |
| service    | name, auth_provider |
| robot      | name, auth_provider |

##### Hashing Rules

1. Normalize key/value pairs:
   - lowercase keys
   - trim whitespace
2. Sort keys lexicographically.
3. Serialize as `key=value` joined by `||`.
4. Append deployment secret salt.
5. Hash deterministically.
6. Encode as Crockford Base32.

Passwords, OAuth tokens, session IDs, and timestamps MUST NOT be included.

##### Guest Entity

Deployments MAY define a default guest entity principal, e.g.:

```
"guest||provider=none"
```

This entity MAY be authorized for minimal updates depending on deployment policy.

Guest entity IDs MUST still be deterministic and auditable.

#### 11.7.1 Derivation Inputs

Entity IDs derive from:

```
(fields from Entity Principal Template Checklist)
+
(deployment_secret_salt)
```

Including a salt:

- Prevents collisions across deployments.
- Allows exporting slugs without exposing sensitive entity attributes.
- Keeps entity IDs stable within a deployment.

#### 11.7.2 Derivation Procedure

1. Gather canonical entity template fields (username, role, etc.).
2. Normalize.
3. Combine with salt (deployment-specific secret).
4. Hash deterministically.
5. Extract 16 Base32 characters.

#### 11.7.3 Notes

- Changing the salt regenerates all entity IDs (not recommended).
- SSO/OIDC/OAuth-derived attributes feed into the same template.

---

### 11.8 Address ID

The `address_id` identifies a specific slug *within* a specific instance.

#### 11.8.1 Definition

```
address_id = slug_id || "||" || instance_id
```

The separator `"||"` is the reference implementation default.

#### 11.8.2 Parsing

- Split at the first occurrence of `"||"`.
- Left side = `slug_id`
- Right side = `instance_id`

Because both slug_id and instance_id have fixed length in the reference implementation, parsing is unambiguous even without a separator; the separator is retained for readability.

---

### 11.9 Predicate Tokens

Predicates used in relationships (Section 8) use a distinct lexical domain.

#### 11.9.1 Syntax

A valid predicate token:

```
[A-Za-z][A-Za-z0-9_]{0,127}
```

Rules:

- ASCII only (no whitespace).
- Case-sensitive.
- Underscores allowed for deployment-specific conventions.
- Maximum length 128 characters.

The server MUST treat predicate tokens as opaque identifiers. The reference implementation defines a canonical grammar that certain daemons MAY recognize (Section 9.5.2), but deployments are not restricted from using custom tokens.

#### 11.9.2 Unknown and Custom Tokens

Predicate tokens not present in a deployment’s optional `predicates` catalog:

- MUST be accepted and stored.
- MUST NOT cause ingest or API calls to fail solely because they are unknown.
- MAY generate warnings (for example, `UNKNOWN_PREDICATE` or `NON_CANONICAL_PREDICATE`).

---

### 11.10 Optional Enhancements (Non-default, Implementation-Defined)

Future revisions or deployments MAY select different design choices.

These are explicitly left open to allow growth without breaking the spec.

#### 11.10.1 Checksum (Optional)

Deployments MAY append a checksum character to:

- slug_id
- instance_id
- entity_id

Checksum MAY use:

- modulo arithmetic,
- BCH codes,
- or any error-detecting scheme.

Checksum MUST NOT break ID parsing rules.

#### 11.10.2 ID Length Variants

Deployments MAY configure:

- 16 characters (default)
- 24 characters (larger scale)
- 32 characters (cryptographic-grade uniqueness)

Longer IDs reduce collision probability; short IDs increase readability.

#### 11.10.3 Alternative Address Separators

These MAY be selected:

- `||` (default)
- `-` (filesystem-friendly)
- No separator (requires fixed-length parsing)

#### 11.10.4 Custom Hash Algorithms

Deployments MAY replace SHA-256 with:

- BLAKE3
- SHA3-256
- SipHash (less preferred for security)

Hashes MUST be stable and deterministic.

All changes MUST be recorded in a deployment configuration file.

---

### 11.11 Collision Handling

Collisions are mathematically improbable with Base32-encoded 80-bit digests (16 chars).

If detected:

- Server MUST reject creation of a duplicate ID with mismatched fields.
- Authors SHOULD modify fields minimally to break the collision.
- Deployments MAY increase ID length for next revision.

Collision detection is part of slug creation but not minimal update.

---

### 11.12 Validation Rules

ID validation MUST check:

- Correct length (per reference or configured policy)
- Allowed characters (Crockford Base32)
- Correct separator usage for `address_id`
- Predicate format (per Section 11.9)
- Non-null, non-empty values

Validation MUST occur on:

- Creation requests
- Update requests (address_id only)
- Relationship upserts
- Markdown ingest
- JSONL ingest

Validation MUST NOT reject unknown predicates.

---

### 11.13 Examples

#### 11.13.1 Slug ID

Input fields:

```
(checklist_slug="oven_startup",
 section="preflight",
 procedure="vent_check",
 action="verify_valve",
 spec="Valve is closed")
```

Derived slug_id (example):

```
ABCD2345EFGH6789
```

#### 11.13.2 Instance ID

Room example → deterministic:

```
ROOM_201    → 9X8Y7W6V5U4T3S2R
```

#### 11.13.3 Address ID

```
ABCD2345EFGH6789||9X8Y7W6V5U4T3S2R
```

#### 11.13.4 Entity ID

```
user=jds role=admin salt=SECRET
→ G7H8J9K1M2N3P4R5
```

---

### 11.14 Summary Table

| ID Type       | Scope | Stability | Default Length | Derived From |
|---------------|--------|-----------|----------------|--------------|
| checklist_id  | Template | High | 16 | checklist_slug |
| slug_id       | Template row | Absolute | 16 | section/procedure/action/spec/instructions |
| instance_id   | Runtime | High | 16 | instance template fields |
| entity_id     | Actor | High | 16 | entity template fields + salt |
| address_id    | Runtime | Derived | — | slug_id + instance_id |
| predicate     | Relationship | Free-form | — | lexical token |

---

### 11.15 Forward Compatibility

This section is expected to evolve in future versions:

- ID lengths may increase.
- Checksums may become standard.
- Entity and instance IDs may gain standardized field sets.
- Address separator rules may tighten for federation.

The core guarantees will remain:

- Determinism
- Stability
- Predictability
- Cross-system portability

---

## 12. Versioning, Template Evolution, and Compatibility

Checklist Assistant is designed to support long-lived checklists, repeated over time, across many deployments. This section defines how **versions** of templates, instances, and identifiers behave under change.

Versioning rules ensure:

- Checklists can evolve without invalidating stored slugs.
- Instances from different eras remain evaluatable.
- Relationships remain stable even as templates change.
- Ingest and API actions remain predictable across revisions.
- A deployment can upgrade Checklist Assistant itself without breaking stored data.

This section clarifies what may change, what must remain stable, and how the system handles forward+backward compatibility.

---

### 12.1 Version Concepts

Checklist Assistant defines four independent version dimensions:

1. **Template Version** Tracks changes in Markdown checklist templates.

2. **Instance Version** Tracks snapshots of generated instances (e.g., Room 201 template v3 vs v4).

3. **Data Model (Schema) Version** Tracks structural changes in Section 6 tables.

4. **API Version** Tracks REST + JSON-RPC interfaces (Section 7).

These dimensions evolve at different rates and are not tied to each other.

---

### 12.2 Template Version (Checklist Version)

A **template version** represents the evolution of a checklist authored in Markdown. It is defined by changes that affect:

- the headings/structure,
- section/procedure/action/spec text,
- relationships,
- instructions or domains.

#### 12.2.1 Template Version Field

Each Markdown file MAY include an optional field:

```yaml
version: 1.0.3
```

But Checklist Assistant does not require this. Instead, Checklist Assistant uses **slug_id regeneration** as the source of truth:

> If a template row changes in a way that affects hashing inputs
> → **slug_id changes** → **this row is considered a new version**.

Thus:

- Template version is implicit, not explicit.
- Checklist Assistant does not track “Template v1, v2, v3” internally.
- Instead, it tracks the **graph of slug_ids over time**.

#### 12.2.2 Allowed and Disallowed Template Modifications

**Allowed without breaking instance data:**

- Reordering procedures
- Changing comments, notes, or metadata sections that are outside any procedure's `instructions`
- Adding/remove relationships (safe; affects evaluation only)

**Changes that create new slug_ids (breaking changes):**

- Modifying checklist name
- Modifying section name
- Modifying procedure name
- Modifying action text
- Modifying spec text
- Modifying instructions text, including material whitespace changes after canonicalization

Checklist Assistant treats these as creation of new procedural atoms.

---

### 12.3 Instance Version

An **instance** (e.g., specific machine, room, run) is tied to the template at the moment of instantiation.

If a template is revised:

- Existing instances **do not** automatically migrate.
- New instances use new slug_ids.
- Old instances remain evaluatable because address_ids remain valid.

This is a deliberate design choice:

> Stored data is immutable.
> Templates evolve.
> Instances preserve the template shape they were created from.

#### 12.3.1 Instance Migration (Optional)

Deployments MAY choose to create migration tools:

- Compare old vs new slug_ids.
- Map corresponding procedural atoms.
- Create updated addresses for new template.
- Migrate stored data selectively.

But Checklist Assistant **does not** define automatic migration.

#### 12.3.2 Instance “Epochs”

Deployments MAY define “epochs” or “cycles.” Example:

- Checklist v1 used in 2025.
- Checklist v2 used in 2026.

Instances from 2025 and 2026 will produce different slug_ids, but can still be evaluated side-by-side.

---

### 12.4 Schema Version and Migration (Data Model Version)

Section 6 defines the recommended SQLite schema.

Deployments MAY evolve schema via:

- Adding new tables
- Adding new columns
- Adding indexes
- Adding history retention options

Deployments MUST NOT:

- Change ID derivation rules for previously created slugs.
- Modify existing slug rows to conform to new template versions.
- Rewrite address_id or entity_id formats retroactively.

#### 12.4.1 Migrating Checklist Assistant Schema

Schema migrations MUST:

- Preserve existing slugs and relationships.
- Preserve history.
- Leave address_id stable.

Schema migrations MAY:

- Add cache tables for evaluation.
- Add auxiliary metadata tables.
- Add indexes for performance.

---

### 12.5 API Version Compatibility

Section 7 defines API versioning rules.

#### 12.5.1 Version Stability Guarantees

API behavior MUST NOT:

- Change minimal update contract semantics.
- Change slug creation semantics.
- Change relationship semantics.
- Change the lexical rules for predicate tokens or the reference canonical grammar without a documented version bump.

These form the “frozen core” of Checklist Assistant (CHAX).

#### 12.5.2 Backward Compatibility

A new API version (e.g., `/api/v2/`) MUST:

- Continue to accept v1 data structures and ID formats.
- Preserve deterministic hashing rules.
- Preserve evaluation correctness.

#### 12.5.3 Forward Compatibility

Older clients MAY:

- Use v1 endpoints against a v2 server.
- Rely on the server to translate responses into stable v1 fields.
- Ignore new fields which v2 may introduce.

---

### 12.6 Evaluation Compatibility (State Machine Versioning)

Reference-daemon evolution must preserve:

- Canonical predicate grammar and single-trigger semantics for recognized canonical tokens (Section 9.5).
- Flag/diagnostic shapes for any exposed evaluation interfaces (Section 9.10), when present.
- The “no hidden writes” rule: any mutation must be an explicit API write attributed to a specific `entity_id`.

Deployments MAY extend evaluation:

- Add new flags.
- Add new recognized canonical token families in future revisions, or deployment-local custom tokens.
- Add optional time-based logic.

All extensions MUST:

- Preserve semantics of existing behavior.
- Remain deterministic.

---

### 12.7 Relationship Evolution and Cross-Version Safety

Relationships stored in templates (`template_relationships`) and overlays (`address_relationships`) MUST remain stable through:

- Schema changes
- Template evolution
- API changes

#### 12.7.1 What Happens When a Target Slug ID Changes?

If template author modifies a row:

- A new slug_id will be generated.
- Old instances still point to old slug_id.
- New instances will use the new slug_id.

Evaluation across versions works because:

- Relationships remain defined for each era.
- Cross-instance comparisons rely on explicit address_id targets.

#### 12.7.2 Handling Deprecated Predicates

Predicates may be deprecated in the `predicates` catalog.

Deprecated predicates:

- MUST NOT break ingest.
- MUST NOT break evaluation.
- MAY generate `DEPRECATED_PREDICATE` warnings.
- MAY be auto-normalized in future revisions.

---

### 12.8 Ingest, Export, and Version Boundaries

Checklist Assistant’s ingest rules from Section 10 ensure version-safe behavior.

#### 12.8.1 Markdown Ingest Across Versions

Markdown representing checklist v1 can be:

- Re-ingested later after template evolves (new slug_ids created).
- Compared to v2 to compute differences (tooling-defined).
- Merged with relationship graphs safely.

Markdown ingest MUST NOT:

- Rewrite stored slugs.
- Rewrite history.
- Rewrite existing relationships.

#### 12.8.2 JSONL Ingest Across Versions

JSONL ingest operates only on:

- `address_id`
- Minimal update fields

Thus JSONL ingest is version-independent.

---

### 12.9 Cross-Deployment Federation

Multiple deployments may exchange:

- slugs
- relationships
- checklists
- entire graphs

To enable this:

- ID rules (Section 11) guarantee deterministic reconstruction.
- Relationship triples are version-resilient.
- Stored data is portable across deployments and eras.

Deployments MUST NOT assume:

- That slug_ids are identical across organizations (different salts/extensions).
- That entity_ids have shared meaning outside local context.

Address_ids remain safe for internal use but may not be portable unless instance_id derivation rules are aligned.

---

### 12.10 Versioning Recommendations for Authors

Authors SHOULD:

- Minimize slug-breaking edits unless intentionally revising procedures.
- Prefer adding new steps over rewriting old ones.
- Maintain clear commit history in the checklist repository.
- Use relationships (template- and/or address-level) to express evolution rather than overwriting.

Authors SHOULD NOT:

- Reuse the same section/procedure names to mean different things across eras.
- Attempt to “force” old instances to map to new slug_ids manually.

---

### 12.11 Versioning Recommendations for Deployments

Deployments SHOULD:

- Store all template files (v1, v2, v3…) for provenance.
- Use JSONL export/import for safe instance migrations.
- Clearly annotate epoch boundaries when changing templates.
- Avoid rewriting slug_ids through manual DB changes.

Deployments MAY:

- Provide migration tooling upstream/downstream.
- Auto-generate “mapping tables” between template eras for reporting.

---

### 12.12 Versioning Recommendations for Implementers

Implementers SHOULD:

- Keep hashing rules stable per Checklist Assistant version.
- Avoid changing Base32 encoders/decoders after production deployments.
- Store derived IDs (slug_id, instance_id, entity_id) permanently.
- Use database migrations for schema changes, never in-place rewrites.

Implementers MAY:

- Introduce optional configuration keys like:

  ```
  id_length
  use_checksum
  hash_algorithm
  separator_policy
  instance_id_fields
  entity_id_fields
  ```

- Offer compatibility layers for older ID policies.

---

### 12.13 Summary of Version Boundaries

| Component | May Change? | Impact | Notes |
|----------|-------------|--------|-------|
| Checklist Template | Yes | New slug_ids | Existing instances unaffected |
| Instance Template | Yes | New instance_ids | Older instances preserved |
| Slug Definition | Yes | Changes slug graph | Relationships version-aware |
| Schema | Yes | Additive only | No rewrite of slugs |
| API | Yes | Backward compatible | Core behavior frozen |
| Evaluation | Yes | Extensions only | Deterministic, non-mutating |
| Relationships | Yes | Version-aware | Cycles/conflicts flagged |
| IDs | No (existing) | Immutable | New policies must not break old IDs |

---

### 12.14 Conclusion

The versioning model ensures that Checklist Assistant remains:

- Stable
- Evolvable
- Backward compatible
- Forward compatible
- Federated
- Auditable
- Deterministic

Templates evolve. Instances persist. IDs remain stable. Evaluation remains valid across eras.

This enables the Checklist Assistant ecosystem to scale from a single machine to large, multi-deployment, multi-year operational environments without loss of integrity.

---

## 13. Reporting and Deployment Flags

### 13.1 Principal display and ID output flags

- `ui.show_instance_principal` (default: false): when true, UIs may render labels as `INSTANCE_ID (principal)`; when false, principals are hidden or hashed while IDs remain visible.
- `ui.show_entity_principal` (default: false): controls whether session menus display the resolved `entity_principal`; high-security deployments keep this off.
- `id.output_mode` (default: `hashed`): when set to `raw_principal`, exports may include raw instance/entity principal strings for offline or field workflows; clients MUST still respect redaction rules for unauthorized callers.

### 13.2 Report generation contract

Deployments MUST provide both a machine-readable export and a human-readable export, but formats are deployment-defined. The reference implementation uses TeX (optionally PDF downstream) plus JSONL, yet other deployments MAY choose different human or machine formats if they preserve the same fields and determinism.

Guidance:

- Human-readable: reference uses TeX; templates may suppress the auto-generated table unless they explicitly request `{{AutoTables}}`/`{{AutoGeneratedTablePages}}`. Deployments may emit PDF, HTML, or other readable outputs.
- Machine-readable: reference uses JSONL header + row lines; alternative structured formats are allowed if they carry the same identifiers/fields and are deterministic.
- Canonical ingest: the JSONL report emitted by the reference implementation is sufficient to reconstitute slugs (address_id + mutable fields) and regenerate human-readable reports later; any alternate machine format MUST preserve that fidelity.
- `reports.include_principals` (default: false) still controls whether principals appear in machine-readable outputs.
- `reports.format.pdf` is advisory; deployments may map it to tex/latex-pdf, fillable PDFs, or another human-readable renderer.
- `reports.minimal_mode_enabled` (default: false) continues to gate reduced outputs (e.g., action/result/comment only).

### 13.3 Output packaging (reference implementation)

- `/api/v1/export/report` writes under the checklist asset root, defaulting to `checklists/<pack>/<checklist>/reports/<instance_id>_<timestamp>/`; deployments may relocate the root with `CHAX_CHECKLISTS_ROOT`.
- `/api/v1/export/report` accepts `format` with `tex` as the default and `html` as the browser-first alternative (`latex` is accepted as a compatibility alias for `tex`).
- Timestamp is ISO 8601 UTC, sanitized for filesystem safety; folder naming aligns human and machine outputs when both are emitted.
- Report generation MUST emit a machine-readable snapshot alongside the human-readable artifact. Reference behavior writes a companion `<checklist>.jsonl` next to the `.tex` or `.html`; alternative structured formats are allowed only if they preserve the same fields and share the identical timestamp/folder.
- Deployments that do not persist a file MUST still record the machine-readable snapshot (for example, append to a history table) keyed by the same timestamp so it can be retrieved later without scraping the `.tex`.
- Reports remain content-neutral: downstream renderers (LaTeX->PDF, HTML, etc.) are outside the scope of the server; the server's job is to write deterministic text artifacts.

### 13.4 Template discovery and fallback

- Templates live under the checklist asset root, defaulting to `checklists/<pack>/<checklist>/templates/`, with format-specific folders such as `templates/tex/` and `templates/html/`.
- Resolution order for TeX: `templates/tex/report.tex`, then `templates/tex/<checklist>.tex`, then `templates/tex/<sanitized_checklist>.tex`, then the same three names directly under `templates/`, then checklist-named subfolders. If none exists, the built-in default TeX template is used.
- Resolution order for HTML mirrors TeX using `.html` and `templates/html/` first; if no matching HTML template exists, the built-in default HTML template is used.
- Templates MAY include an optional header comment for provenance, e.g.:

  ```
  % CHAX-REPORT-TEMPLATE {"checklist": "maintenance_a", "applies_to": ["maintenance_a"], "version": "2025-12-13"}
  ```

This is advisory metadata for humans and tooling; the server does not block on it.
- The generator prepends a standard metadata comment block (checklist, instance_id, instance_principal, generated_at, row_count, template_used/name/path, output_path) to every emitted report so provenance survives even when the template omits it.

### 13.5 Template variables and escaping

- Templates use simple `{{token}}` replacement; tokens match `[A-Za-z0-9_-]+` (no control flow, no includes).
- Canonical tokens (align with slug/runtime field names): `checklist`, `instance_id`, `instance_principal` (`unknown` when withheld), `timestamp` (ISO UTC), `row_count` (rows rendered after NA/empty filtering and template suppression), `template_name`, `template_used` (`true|false`), `template_path`, `output_dir`, `output_path`, `AutoTables`, `AutoGeneratedTablePages`.
- Compatibility aliases provided by the reference implementation: `generated_at` (alias of `timestamp`), `title`/`checklist_title` (alias of `checklist`), `report_filename`, and `jsonl_filename`. Deployments MAY add additional aliases, but canonical names above remain normative for interoperability.
- Row-level injection (implemented): address- and slug-scoped tokens are supported so templates can be reusable across instances. Two formats are accepted:
  - Underscored: `slug_<address_id>_<field>` or `slug_<slug_id>_<field>`
  - Dashed (TeX-friendly alias): `slug-<address_id>-<field>` or `slug-<slug_id>-<field>` Fields: `procedure|action|spec|result|status|comment|instructions|instance_principal`. Using any of these tokens removes that row from the auto-generated tables by default. To inject the field **and** keep the row in the auto tables, use the keep suffix:
  - `slug_<address_id>_<field>_keep` or `slug_<slug_id>_<field>_keep`
  - `slug-<address_id>-<field>-keep` or `slug-<slug_id>-<field>-keep`
- Optional slug-lineage aliasing: deployments MAY offer a report-time option to resolve slug/address tokens for predecessor slug_ids to the latest slug based on `slugSuccessor` template relationships. Ambiguous or cyclic chains SHOULD yield warnings. Reference implementation: `/api/v1/export/report` accepts a boolean `use_latest_slug_lineage` in the request body.
- All tokens are escaped for the target renderer before substitution except `AutoTables`/`AutoGeneratedTablePages`, which are emitted as raw renderer-native markup (`LaTeX` for TeX templates, HTML for HTML templates).
- Unknown tokens render as empty strings; template authors should stick to the vocabulary above or the row-level vocabulary above.

### 13.6 Auto-generated table semantics

- Auto tables render per section in `address_order` (`slug_order` table); if unset, fall back to `section/procedure/action`.
- Rows with `status` empty, `NA`, or `N/A` are suppressed from the auto tables; `row_count` reports the number of rendered rows after this filter.
- Section labels are slugified for LaTeX labels; content is escaped.
- Templates that omit `{{AutoTables}}`/`{{AutoGeneratedTablePages}}` suppress the auto tables entirely; the default template always includes them.

### 13.7 Default template metadata block

- The built-in fallback templates render visible metadata near the top of the report: checklist, instance_id, instance_principal, generated_at, rendered row count, and the template name (`default` when no custom template is present).
- This ensures a baseline human-readable report even when no template exists or a template chooses to omit the auto tables.

### 13.8 Custom template opt-in (instance-level toggle)

- Deployments MAY gate custom template usage per instance via a dedicated checklist row (e.g., `Custom report template`). A `Pass` state enables the custom report template for that instance; any other state falls back to the default auto-table report.
- Templates remain layout-agnostic: they may place injected fields anywhere, rely solely on auto tables, or mix both. The server only interprets tokens; it does not enforce pagination or structure.

### 13.9 Checklist-local scripts and automation logs (reference implementation)

- Checklist-local scripts live under the checklist asset root, defaulting to `checklists/<pack>/<checklist>/scripts/`.
- A `scripts.json` manifest MAY define the script catalog. Each enabled entry names a relative script path and may provide a label, description, default arguments, `start_args`, `stop_args`, and a command override. If no manifest is present, the reference implementation auto-discovers runnable scripts.
- The reference implementation recognizes common local runner formats, including PowerShell, Python, shell, batch/cmd, and executable files. Deployments SHOULD narrow this set when operating in a restricted environment.
- `GET /api/v1/workspace/scripts` returns the script catalog for the selected checklist, including validity and diagnostic details suitable for UI/MCP surfacing.
- `POST /api/v1/workspace/scripts/run` launches a selected script with explicit `args`, optional `instance_id` and `instance_principal`, and a `dry_run` mode. The launched process receives checklist context through environment variables such as `CHAX_HOST`, `CHAX_PORT`, `CHAX_BASE_URL`, `CHAX_CHECKLIST`, `CHAX_PACK`, `CHAX_SCRIPT_ID`, `CHAX_INSTANCE_ID`, and `CHAX_INSTANCE_PRINCIPAL`.
- `/api/v1/workspace/scripts/run` treats launched script stdout/stderr as checklist-local runtime artifacts rather than global server artifacts.
- Reference behavior writes script logs under the checklist asset root, defaulting to `checklists/<pack>/<checklist>/logs/`, using timestamped filenames derived from pack, checklist, and `script_id` (for example `script-name-<timestamp>.out.log`).
- Deployments SHOULD keep these scripts and logs beside the checklist asset pack so deleting or sharing the checklist folder has clear, local ownership semantics.
- The server MAY still keep its own operational logs under a global runtime directory; checklist-local script logs are different because they can contain deployment-specific payloads emitted by that checklist's automation.

### 13.10 Checklist asset pack layout (reference implementation)

The reference implementation uses `checklists/<pack>/<checklist>/` as the canonical asset root for workspace-managed checklist material. `<pack>` is a logical owner or collection name, and `<checklist>` is the checklist folder. The checklist folder is the portability unit: sharing, archiving, testing, or deleting a checklist should generally operate on that folder and its local artifacts.

- `checklist.md` is the canonical human-authored Markdown source for the checklist template. A deployment MAY also keep `checklist.json` or other derived forms beside it, but those derived forms should not become a competing authoring surface.
- `data/` contains checklist-local lookup tables, prefill datasets, parameter maps, and other structured inputs used by predicates, scripts, reports, or import/export helpers.
- `templates/tex/` and `templates/html/` contain report templates. Fillable PDF/FDF support may use `templates/report.pdf` and `templates/report.fdf`.
- `media/` and `img/` contain portable visual assets referenced by checklist instructions or report templates. Relative paths are preferred.
- `scripts/` contains checklist-local automation plus optional `scripts.json` metadata as described in Section 13.9.
- `reports/`, `saves/`, and `logs/` are runtime artifact folders owned by the checklist pack. They are useful for local audit/debug work but are often ignored by version control.
- `docs/` may contain checklist-local operator notes, customer/vendor references, connector examples, generated manuals, or other material that should travel with the checklist but is not part of the public project documentation set.

Reference workspace helpers export and restore checklist material using this structure. The environment variable `CHAX_CHECKLISTS_ROOT` MAY override the root folder for a deployment, but public examples and documentation should use `checklists/<pack>/<checklist>/` unless describing that override explicitly.

Private customer or site-specific material SHOULD remain inside the relevant private pack under `checklists/<private-pack>/...`. Public specification examples SHOULD stay generic; when a private deployment proves a reusable feature, create a generic demo pack or generic docs example instead of requiring readers to inspect private material.

### 13.11 Transportable asset-pack archives (reference implementation)

The reference implementation uses `.chk` as the preferred transport extension for checklist asset packs. A `.chk` file is a 7-Zip-format archive; `.7z` and `.zip` archives are also recognized for interoperability with normal archive tooling.

Archive export packages one checklist folder rooted at `checklists/<pack>/<checklist>/` and preserves the pack/checklist folder structure inside the archive. Archive import restores the file tree first, then imports the restored `checklist.md` into the runtime database with the same source/pack/checklist ownership behavior as workspace Markdown import.

The database is not a blob store for the whole archive. It receives the checklist runtime surface (rows, relationships, order, address identity, and ownership metadata), while sidecar assets such as lookup CSVs, images, report templates, scripts, local docs, reports, saves, and logs remain files in the restored checklist folder.

---

## 14. Resolved Direction and Remaining Follow-up

This section records direction from the current implementation review without turning private working notes into public specification text. Normative behavior belongs in the sections above; this section is a compact map for future cleanup and planning.

### 14.1 Resolved Direction

- **Asset root:** the public term is `checklists/<pack>/<checklist>/`; older asset-root terminology is historical and should not appear in public specification prose.
- **Markdown preflight:** workspace template discovery should pre-parse Markdown and surface machine-readable warnings before import. The reference endpoint is `GET /api/v1/workspace/markdown/templates`.
- **Predicate families:** the public predicate grammar must cover status propagation, `BoolVerify...` deterministic verification, slot-field `SearchPrefill`, and slot-field `PropagateValidated` because all four are active implementation concepts in the reference code.
- **Spec/result evaluation:** interval notation, comparator syntax, unit-aware quantities, boolean checks, scalar checks, and text fallback are operational. New quantitative authoring should prefer explicit units and interval/comparator syntax over strict `qty:` notation.
- **Checklist-local scripts:** custom checklist automation is currently represented by checklist-local `scripts/`, optional `scripts.json`, `/api/v1/workspace/scripts`, `/api/v1/workspace/scripts/run`, and checklist-local logs.
- **Mutation path:** HTTP and MCP write contracts remain the canonical mutation path for web clients, agent clients, terminal tools, and automation. Future SSE or websocket features should notify about state changes, not introduce a competing write path.
- **Primary executable:** local use should continue to work from the primary application executable, with CLI flags and help evolving there rather than scattering startup behavior across unrelated entrypoints.
- **Local and hosted modes:** local browser/server use is the stable baseline. LAN or hosted deployments are plausible and partially proven, but public guidance still needs explicit security, token, and operator-checklist work before broad recommendation.
- **Public examples:** specification examples should remain generic. Project-owned demo packs may be public; private client packs should stay out of public examples unless redacted into generic reference material.

### 14.2 Remaining Follow-up

- **CSV checklist authoring:** a future `checklist.csv` format may be useful for spreadsheet and database workflows. It should be designed as a drop-in authoring/import format that normalizes to the same slug, relationship, and runtime model as `checklist.md`; it is not normative in this version.
- **Relationship authoring ergonomics:** tool-assisted authoring and import/export round trips are now the practical baseline for large checklists. Temporary row references or richer aliases can be explored, but they should normalize quickly to stable IDs.
- **Hosted security:** public or non-loopback deployments need a hardened authorization contract, entity-principal mapping guidance, secret handling rules, and a deployment checklist before the spec should present them as routine.
- **Reference examples for client-derived features:** when a private deployment drives a new general feature, create a generic example in a public demo pack so future implementers can learn the behavior without reading private checklists.
